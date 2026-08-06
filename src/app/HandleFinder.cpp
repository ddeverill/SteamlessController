#include "HandleFinder.h"
#include <winternl.h>  // UNICODE_STRING, NTSTATUS
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace HandleFinder {
namespace {

// ---------------------------------------------------------------------------
// Undocumented surface. Resolved by name at run time rather than linked, so a
// Windows build that no longer exports these degrades to an empty result
// instead of failing to start the process.
// ---------------------------------------------------------------------------

constexpr NTSTATUS kStatusInfoLengthMismatch  = static_cast<NTSTATUS>(0xC0000004L);
constexpr ULONG    kSystemExtendedHandleInfo  = 64;
constexpr ULONG    kObjectNameInformation     = 1;

struct SYSTEM_HANDLE_ENTRY_EX {
    PVOID     Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG     GrantedAccess;
    USHORT    CreatorBackTraceIndex;
    USHORT    ObjectTypeIndex;
    ULONG     HandleAttributes;
    ULONG     Reserved;
};

struct SYSTEM_HANDLE_INFORMATION_EX {
    ULONG_PTR             NumberOfHandles;
    ULONG_PTR             Reserved;
    SYSTEM_HANDLE_ENTRY_EX Handles[1];
};

struct OBJECT_NAME_INFO {
    UNICODE_STRING Name;
    WCHAR          Buffer[1];
};

using PfnQuerySystemInformation = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using PfnQueryObject            = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

struct NtApi {
    PfnQuerySystemInformation QuerySystemInformation = nullptr;
    PfnQueryObject            QueryObject            = nullptr;

    bool Valid() const { return QuerySystemInformation && QueryObject; }
};

const NtApi& Nt() {
    static const NtApi api = [] {
        NtApi a{};
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
            a.QuerySystemInformation = reinterpret_cast<PfnQuerySystemInformation>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQuerySystemInformation")));
            a.QueryObject = reinterpret_cast<PfnQueryObject>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryObject")));
        }
        return a;
    }();
    return api;
}

// The NT name of whatever a handle refers to, e.g. "\Device\0000009e".
// Empty when it cannot be determined.
std::wstring ObjectName(HANDLE h) {
    if (!Nt().Valid()) return L"";

    std::vector<BYTE> buf(2048);
    for (int attempt = 0; attempt < 2; ++attempt) {
        ULONG needed = 0;
        const NTSTATUS st = Nt().QueryObject(h, kObjectNameInformation, buf.data(),
                                             static_cast<ULONG>(buf.size()), &needed);
        if (st == kStatusInfoLengthMismatch && needed > buf.size()) {
            buf.resize(needed);
            continue;
        }
        if (st < 0) return L"";

        const auto* info = reinterpret_cast<const OBJECT_NAME_INFO*>(buf.data());
        if (!info->Name.Buffer || info->Name.Length == 0) return L"";
        return std::wstring(info->Name.Buffer, info->Name.Length / sizeof(WCHAR));
    }
    return L"";
}

std::vector<BYTE> SnapshotHandles() {
    if (!Nt().Valid()) return {};

    // The table grows between the sizing call and the read, so ask for room to
    // spare and retry rather than trusting the reported length.
    ULONG size = 1u << 20;
    for (int attempt = 0; attempt < 6; ++attempt) {
        std::vector<BYTE> buf(size);
        ULONG needed = 0;
        const NTSTATUS st = Nt().QuerySystemInformation(kSystemExtendedHandleInfo,
                                                        buf.data(), size, &needed);
        if (st >= 0) return buf;
        if (st != kStatusInfoLengthMismatch) return {};
        size = (needed > size ? needed : size) * 2;
    }
    return {};
}

std::wstring ProcessImageName(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return L"";

    wchar_t path[MAX_PATH] = {};
    DWORD   len            = ARRAYSIZE(path);
    std::wstring name;
    if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
        name = path;
        const size_t slash = name.find_last_of(L'\\');
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
    }
    CloseHandle(proc);
    return name;
}

struct Candidate {
    DWORD  pid    = 0;
    HANDLE handle = nullptr;
};

// Results are shared with scan threads that may outlive the caller's patience,
// so they live behind a shared_ptr and a lock.
struct ScanState {
    std::mutex             mutex;
    std::vector<Holder>    holders;
    std::vector<Candidate> candidates;
    std::wstring           targetName;
    std::atomic<size_t>    next{0};
    std::atomic<unsigned>  examined{0};
    std::atomic<unsigned>  queried{0};
    // Candidates fully dealt with. Distinct from `next`, which only counts
    // work claimed: a worker blocked inside NtQueryObject has claimed its
    // candidate but not finished it, and that gap is the whole point.
    std::atomic<size_t>    processed{0};
};

// Claimed one at a time, deliberately. Blocks of 64 were tried first, on the
// theory that the handle table is grouped by process so a block would hit the
// per-worker OpenProcess cache — but a worker that blocks partway through a
// block strands the rest of it: claimed, never processed, so the scan can never
// report completion and those candidates are never examined by anyone.
// Measured, that lost ~200 candidates and one of the two known holders.
constexpr size_t kClaimBlock = 1;

// Flatten the handle table into a work list. Done up front and on the caller's
// thread so the snapshot buffer does not have to outlive the workers.
void CollectCandidates(const std::vector<BYTE>& snapshot, USHORT fileTypeIndex,
                       DWORD selfPid, std::vector<Candidate>& out) {
    if (snapshot.empty()) return;

    const auto* table = reinterpret_cast<const SYSTEM_HANDLE_INFORMATION_EX*>(snapshot.data());
    const ULONG_PTR count = table->NumberOfHandles;

    for (ULONG_PTR i = 0; i < count; ++i) {
        const SYSTEM_HANDLE_ENTRY_EX& e = table->Handles[i];
        if (e.ObjectTypeIndex != fileTypeIndex) continue;

        const DWORD pid = static_cast<DWORD>(e.UniqueProcessId);
        if (pid == selfPid || pid == 0 || pid == 4) continue;  // ours, idle, System

        out.push_back(Candidate{pid, reinterpret_cast<HANDLE>(e.HandleValue)});
    }
}

// Examine one candidate handle. `proc`/`procPid` are the caller's one-entry
// OpenProcess cache, carried across calls because consecutive candidates
// usually belong to the same process.
void ProcessOne(const std::shared_ptr<ScanState>& state, const Candidate& c,
                HANDLE& proc, DWORD& procPid) {
    const DWORD pid = c.pid;

    // Already recorded this process? One hit is all we need from it.
    {
        std::lock_guard<std::mutex> lk(state->mutex);
        for (const auto& h : state->holders)
            if (h.pid == pid) return;
    }

    if (pid != procPid) {
        if (proc) CloseHandle(proc);
        proc    = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
        procPid = pid;
    }
    if (!proc) return;  // protected or higher-integrity process

    HANDLE dup = nullptr;
    if (!DuplicateHandle(proc, c.handle, GetCurrentProcess(), &dup, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return;

    // Named pipes are the classic NtQueryObject hang, and they share the File
    // object type with devices. Screening by type costs one cheap call and
    // removes almost all of the exposure — it also drops FILE_TYPE_DISK, which
    // is the bulk of every handle table.
    //
    // FILE_TYPE_UNKNOWN has to stay in this test, however tempting it looks:
    // measured on this controller, a handle opened with zero access reports
    // UNKNOWN rather than CHAR, so requiring CHAR would skip the very handles
    // being looked for. Pipes report FILE_TYPE_PIPE either way, so the hang
    // protection survives.
    const DWORD fileType = GetFileType(dup);
    if (fileType == FILE_TYPE_CHAR || fileType == FILE_TYPE_UNKNOWN) {
        // Counted before the call, not after: if this is one of the ones that
        // blocks, the count is what shows how far the scan got.
        state->queried.fetch_add(1);
        if (_wcsicmp(ObjectName(dup).c_str(), state->targetName.c_str()) == 0) {
            Holder h{pid, ProcessImageName(pid)};
            if (h.image.empty()) h.image = L"(unknown)";
            std::lock_guard<std::mutex> lk(state->mutex);
            state->holders.push_back(h);
        }
    }
    CloseHandle(dup);
}

// One worker. Several run at once because a handful of NtQueryObject calls
// block for seconds at a time, and sequentially that is fatal: measured, 494
// name lookups took 8s and the scan died on the deadline having found only the
// first holder. Spreading the work means a blocked call costs one worker rather
// than the whole scan.
void ScanWorker(std::shared_ptr<ScanState> state) {
    HANDLE lastProc    = nullptr;
    DWORD  lastProcPid = 0;

    for (;;) {
        const size_t block = state->next.fetch_add(kClaimBlock);
        if (block >= state->candidates.size()) break;
        const size_t end = (block + kClaimBlock < state->candidates.size())
                               ? block + kClaimBlock
                               : state->candidates.size();

        for (size_t index = block; index < end; ++index) {
            state->examined.fetch_add(1);
            ProcessOne(state, state->candidates[index], lastProc, lastProcPid);
            state->processed.fetch_add(1);
        }
    }

    if (lastProc) CloseHandle(lastProc);
}

}  // namespace

ScanReport FindDeviceHolders(const std::wstring& devicePath, DWORD budgetMs) {
    ScanReport report;
    const ULONGLONG startTick = GetTickCount64();
    if (!Nt().Valid()) return report;

    // Open the device ourselves purely to learn its NT object name — asking the
    // system beats guessing how an interface path maps to \Device\... Zero
    // access is granted regardless of how anyone else holds it, so this cannot
    // fail for the very reason we are here.
    HANDLE self = CreateFileW(devicePath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (self == INVALID_HANDLE_VALUE) return report;

    const std::wstring targetName = ObjectName(self);
    report.targetName = targetName;

    // Our own entry also supplies the File type index, which is not a fixed
    // constant across Windows versions.
    // Taken once and reused for both jobs: it was being captured twice, and on
    // a live system that is a multi-megabyte walk each time.
    USHORT            fileTypeIndex = 0;
    bool              haveIndex     = false;
    const DWORD       selfPid       = GetCurrentProcessId();
    std::vector<BYTE> snapshot;
    if (!targetName.empty()) {
        snapshot = SnapshotHandles();
        if (!snapshot.empty()) {
            const auto* table =
                reinterpret_cast<const SYSTEM_HANDLE_INFORMATION_EX*>(snapshot.data());
            for (ULONG_PTR i = 0; i < table->NumberOfHandles; ++i) {
                const SYSTEM_HANDLE_ENTRY_EX& e = table->Handles[i];
                if (static_cast<DWORD>(e.UniqueProcessId) == selfPid
                        && reinterpret_cast<HANDLE>(e.HandleValue) == self) {
                    fileTypeIndex = e.ObjectTypeIndex;
                    haveIndex     = true;
                    break;
                }
            }
        }
    }
    CloseHandle(self);
    report.typeIndex = fileTypeIndex;

    if (targetName.empty() || !haveIndex) {
        report.elapsedMs = static_cast<DWORD>(GetTickCount64() - startTick);
        return report;
    }

    auto state = std::make_shared<ScanState>();
    state->targetName = targetName;
    CollectCandidates(snapshot, fileTypeIndex, selfPid, state->candidates);
    snapshot.clear();
    snapshot.shrink_to_fit();

    if (state->candidates.empty()) {
        report.completed = true;
        report.elapsedMs = static_cast<DWORD>(GetTickCount64() - startTick);
        return report;
    }

    // The scan runs on worker threads so a blocked name lookup cannot take the
    // caller with it. If the deadline passes we report what was found so far and
    // abandon the threads: they only touch the shared state, which outlives them
    // by shared_ptr, and the helper process this runs in exits moments later.
    unsigned workerCount = std::thread::hardware_concurrency();
    if (workerCount < 4)  workerCount = 4;
    if (workerCount > 16) workerCount = 16;

    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (unsigned i = 0; i < workerCount; ++i)
        workers.emplace_back(ScanWorker, state);

    // Wait on the work, not on the threads. Waiting for every thread to return
    // means one worker blocked inside NtQueryObject reports the whole scan as
    // failed even when all the candidates were dealt with and every holder
    // found — which is exactly what the first parallel run did.
    //
    // processed == total also guarantees no worker is still inside a candidate,
    // so joining is safe; short of that, threads may be blocked forever and
    // only detaching is.
    const size_t     total    = state->candidates.size();
    const ULONGLONG  deadline = GetTickCount64() + budgetMs;
    bool             finished = false;
    while (GetTickCount64() < deadline) {
        if (state->processed.load() >= total) { finished = true; break; }
        Sleep(20);
    }

    for (auto& w : workers) {
        if (finished) w.join();
        else          w.detach();
    }

    std::lock_guard<std::mutex> lk(state->mutex);
    report.holders   = state->holders;
    report.examined  = state->examined.load();
    report.queried   = state->queried.load();
    report.completed = finished;
    report.elapsedMs = static_cast<DWORD>(GetTickCount64() - startTick);
    return report;
}

std::wstring DescribeHolders(const std::vector<Holder>& holders) {
    std::wstring out;
    for (const auto& h : holders) {
        if (!out.empty()) out += L"; ";
        out += h.image;
        out += L" (";
        out += std::to_wstring(h.pid);
        out += L")";
    }
    return out;
}

}  // namespace HandleFinder
