#include "InputInjection.h"
#include "EventLog.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace {

// A wedged foreground window refuses every event at report rate, so the log
// gets one line per burst rather than one per event.
constexpr uint64_t kLogGapMs = 5000;

// The balloon is a warning, not a running commentary: alt-tabbing in and out of
// an elevated window must not produce one notification per trip.
constexpr uint64_t kAlertGapMs = 60000;

constexpr DWORD kIntegrityUnknown = 0xFFFFFFFFu;

std::atomic<uint64_t> g_refused{0};        // refusals since the last summary
std::atomic<uint64_t> g_refusedTotal{0};   // refusals since the last recovery
std::atomic<uint64_t> g_lastRefusalLogMs{0};
std::atomic<uint64_t> g_lastStuckLogMs{0};
std::atomic<bool>     g_refusing{false};

// True at most once per kLogGapMs, tracked per call site.
bool DueForLog(std::atomic<uint64_t>& lastMs) {
    const uint64_t now  = GetTickCount64();
    uint64_t       last = lastMs.load(std::memory_order_relaxed);
    if (last != 0 && now - last < kLogGapMs) return false;
    // A racing thread that wins here logs instead of this one; either way the
    // line is written once.
    return lastMs.compare_exchange_strong(last, now, std::memory_order_relaxed);
}

const wchar_t* IntegrityName(DWORD rid) {
    if (rid == kIntegrityUnknown)                return L"unreadable";
    if (rid >= SECURITY_MANDATORY_SYSTEM_RID)    return L"System";
    if (rid >= SECURITY_MANDATORY_HIGH_RID)      return L"High/elevated";
    if (rid >= SECURITY_MANDATORY_MEDIUM_RID)    return L"Medium";
    if (rid >= SECURITY_MANDATORY_LOW_RID)       return L"Low";
    return L"Untrusted";
}

DWORD ProcessIntegrity(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return kIntegrityUnknown;

    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);

    DWORD rid = kIntegrityUnknown;
    if (size > 0) {
        std::vector<uint8_t> buf(size);
        auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
        if (GetTokenInformation(token, TokenIntegrityLevel, label, size, &size)) {
            const UCHAR* count = GetSidSubAuthorityCount(label->Label.Sid);
            if (count && *count > 0) {
                rid = *GetSidSubAuthority(label->Label.Sid,
                                          static_cast<DWORD>(*count - 1));
            }
        }
    }
    CloseHandle(token);
    return rid;
}

// This process's own level never changes, so it is worth asking once.
DWORD OwnIntegrity() {
    static const DWORD own = ProcessIntegrity(GetCurrentProcess());
    return own;
}

// ---------------------------------------------------------------------------
// Blocked-state tracking
// ---------------------------------------------------------------------------

std::mutex   g_stateMutex;
InputInjection::AlertFn g_alertFn;         // guarded by g_stateMutex
std::wstring g_blockerLogged;              // blocker named in the log right now
std::wstring g_alertBlocker;               // blocker the last balloon named
uint64_t     g_lastAlertMs = 0;

// Foreground lookups are cached against the window they describe: injection
// happens at report rate, foreground changes do not, and a process cannot
// change integrity level while it lives.
HWND         g_cachedHwnd    = nullptr;    // guarded by g_stateMutex
DWORD        g_cachedPid     = 0;
bool         g_cachedBlocked = false;
std::wstring g_cachedName;

// Exe name of the foreground window's process when it outranks this one, so
// anything injected now is discarded. Empty when injection can land.
//
// A process that cannot be opened at all — protected processes, in practice
// anti-cheat — is deliberately NOT called blocked: its level is unknown, and
// guessing would put a wrong name in front of the user. The stuck-cursor check
// still catches it, and LogEnvironment records that the process was unreadable.
std::wstring ForegroundBlocker(DWORD& pidOut) {
    pidOut = 0;

    const HWND fg = GetForegroundWindow();
    if (!fg) return L"";  // secure desktop or lock screen; nothing to name

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return L"";
    pidOut = pid;

    std::lock_guard<std::mutex> lk(g_stateMutex);
    if (fg == g_cachedHwnd && pid == g_cachedPid)
        return g_cachedBlocked ? g_cachedName : std::wstring();

    g_cachedHwnd    = fg;
    g_cachedPid     = pid;
    g_cachedBlocked = false;
    g_cachedName.clear();

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return L"";

    const DWORD integrity = ProcessIntegrity(proc);

    wchar_t image[MAX_PATH] = L"";
    DWORD   imageLen        = MAX_PATH;
    if (!QueryFullProcessImageNameW(proc, 0, image, &imageLen))
        wcscpy_s(image, L"(unknown)");
    CloseHandle(proc);

    const DWORD own = OwnIntegrity();
    if (integrity == kIntegrityUnknown || own == kIntegrityUnknown
            || integrity <= own)
        return L"";

    const wchar_t* leaf = wcsrchr(image, L'\\');
    g_cachedName    = leaf ? leaf + 1 : image;
    g_cachedBlocked = true;
    return g_cachedName;
}

// Records the blocked state changing: one log line per blocker rather than per
// event, and a balloon the first time each blocker gets in the way.
void NoteBlockState(const std::wstring& blocker, DWORD pid, const char* what) {
    std::wstring            startedBlocking, stoppedBlocking;
    InputInjection::AlertFn alert;

    {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        if (!blocker.empty()) {
            if (blocker != g_blockerLogged) {
                startedBlocking = blocker;
                g_blockerLogged = blocker;

                const uint64_t now = GetTickCount64();
                if (blocker != g_alertBlocker || now - g_lastAlertMs >= kAlertGapMs) {
                    g_alertBlocker = blocker;
                    g_lastAlertMs  = now;
                    alert          = g_alertFn;
                }
            }
        } else if (!g_blockerLogged.empty()) {
            stoppedBlocking = g_blockerLogged;
            g_blockerLogged.clear();
        }
    }

    if (!startedBlocking.empty()) {
        EventLog::Write("INJECT: BLOCKED (%s) — the foreground window belongs to "
                        "%ls (pid=%lu), which runs at a higher integrity level; "
                        "Windows accepts each event and then discards it",
                        what, startedBlocking.c_str(), pid);
        InputInjection::LogEnvironment("state while blocked");
    }
    if (!stoppedBlocking.empty())
        EventLog::Write("INJECT: unblocked — %ls is no longer the foreground window",
                        stoppedBlocking.c_str());

    // Outside the lock: the callback is the tray's, and it is not this
    // module's business what it does before returning.
    if (alert) {
        std::wstring text = startedBlocking;
        text += L" is running as administrator, and Windows does not let "
                L"SteamlessController send input while it is the active window. "
                L"The trackpad and buttons work again as soon as you switch to "
                L"another window or close it.";
        alert(L"Controller input is being blocked", text);
    }
}

// "explorer.exe pid=1234 integrity=Medium", or why that could not be answered.
// A process this app cannot open at all is a finding in itself: protected
// processes (anti-cheat, in practice) refuse even limited query access, and
// those are also the processes most likely to be filtering injected input.
std::wstring DescribeForeground(DWORD& integrityOut) {
    integrityOut = kIntegrityUnknown;

    const HWND fg = GetForegroundWindow();
    if (!fg) {
        // No foreground window at all: a UAC prompt or the lock screen has
        // taken the input desktop, and nothing injected reaches the user's
        // session until it goes away.
        return L"(none — no foreground window; a UAC prompt, the lock screen "
               L"or a desktop switch owns input)";
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        wchar_t out[128];
        swprintf_s(out, L"pid=%lu (cannot open, err=%lu — protected process?)",
                   pid, GetLastError());
        return out;
    }

    wchar_t image[MAX_PATH] = L"";
    DWORD   imageLen        = MAX_PATH;
    if (!QueryFullProcessImageNameW(proc, 0, image, &imageLen))
        wcscpy_s(image, L"(unknown)");

    // Leaf name only — the full path adds nothing here and can carry the
    // user's name in it.
    const wchar_t* leaf = wcsrchr(image, L'\\');
    leaf = leaf ? leaf + 1 : image;

    integrityOut = ProcessIntegrity(proc);
    CloseHandle(proc);

    wchar_t out[256];
    swprintf_s(out, L"%s pid=%lu integrity=%s",
               leaf, pid, IntegrityName(integrityOut));
    return out;
}

// The cursor clip, when a window has narrowed it. Games set this and do not
// always restore it, which pins the cursor while injection still succeeds.
std::wstring DescribeClip() {
    RECT clip{};
    if (!GetClipCursor(&clip)) {
        wchar_t out[64];
        swprintf_s(out, L"unreadable (err=%lu)", GetLastError());
        return out;
    }

    RECT screen{};
    screen.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    screen.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    screen.right  = screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    screen.bottom = screen.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    // The bounds go in either way: a cursor sitting on an edge of the desktop
    // cannot move further that way, which reads as "stuck" without them.
    wchar_t out[160];
    if (EqualRect(&clip, &screen)) {
        swprintf_s(out, L"none (screen is (%ld,%ld)-(%ld,%ld))",
                   screen.left, screen.top, screen.right, screen.bottom);
    } else {
        swprintf_s(out, L"CONFINED to (%ld,%ld)-(%ld,%ld) of screen "
                        L"(%ld,%ld)-(%ld,%ld)",
                   clip.left, clip.top, clip.right, clip.bottom,
                   screen.left, screen.top, screen.right, screen.bottom);
    }
    return out;
}

std::wstring DescribeCursor() {
    POINT pt{};
    if (!GetCursorPos(&pt)) {
        // Fails when this thread's desktop is not the input desktop — the
        // session is locked, or a secure desktop is up.
        wchar_t out[96];
        swprintf_s(out, L"unreadable (err=%lu — not on the input desktop)",
                   GetLastError());
        return out;
    }
    wchar_t out[64];
    swprintf_s(out, L"(%ld,%ld)", pt.x, pt.y);
    return out;
}

}  // namespace

namespace InputInjection {

void LogEnvironment(const char* reason) {
    DWORD fgIntegrity = kIntegrityUnknown;
    const std::wstring fg   = DescribeForeground(fgIntegrity);
    const DWORD        own  = OwnIntegrity();
    const std::wstring clip = DescribeClip();
    const std::wstring cur  = DescribeCursor();

    const bool outranked = fgIntegrity != kIntegrityUnknown
                        && own         != kIntegrityUnknown
                        && fgIntegrity > own;

    EventLog::Write("INJECT: %s — foreground=%ls, us=integrity=%ls%s, "
                    "cursorClip=%ls, cursor=%ls",
                    reason, fg.c_str(), IntegrityName(own),
                    outranked ? " — FOREGROUND OUTRANKS US, injected input is "
                                "blocked (UIPI) until focus moves or it closes"
                              : "",
                    clip.c_str(), cur.c_str());
}

void SetAlertCallback(AlertFn fn) {
    std::lock_guard<std::mutex> lk(g_stateMutex);
    g_alertFn = std::move(fn);
}

bool Send(const INPUT& input, const char* what) {
    // Asked before sending, and sent either way: the check is what reports the
    // fault, not a gate on the event. If it is ever wrong about a system, the
    // input still goes out exactly as it did before.
    DWORD             blockerPid = 0;
    const std::wstring blocker   = ForegroundBlocker(blockerPid);
    NoteBlockState(blocker, blockerPid, what);

    INPUT copy = input;  // SendInput takes a non-const array
    if (SendInput(1, &copy, sizeof(INPUT)) == 1) {
        if (g_refusing.exchange(false)) {
            EventLog::Write("INJECT: accepted again after %llu refused event(s)",
                            g_refusedTotal.exchange(0));
            g_refused.store(0);
            g_lastRefusalLogMs.store(0);
        }
        return true;
    }

    const DWORD err = GetLastError();
    g_refusing.store(true);
    g_refusedTotal.fetch_add(1);
    const uint64_t burst = g_refused.fetch_add(1) + 1;

    if (DueForLog(g_lastRefusalLogMs)) {
        g_refused.store(0);
        EventLog::Write("INJECT: SendInput REFUSED (%s, err=%lu%s) — %llu event(s) "
                        "dropped since the last line",
                        what, err,
                        err == ERROR_ACCESS_DENIED ? " ACCESS_DENIED" : "",
                        burst);
        LogEnvironment("state at refusal");
    }
    return false;
}

void LogCursorNotMoving(long travelPx, POINT at) {
    {
        // A known blocker already accounts for a still cursor, and has said so
        // in better terms. What is left for this check is the causes nothing
        // else can see: a cursor clip, or a hook eating injected input.
        std::lock_guard<std::mutex> lk(g_stateMutex);
        if (!g_blockerLogged.empty()) return;
    }
    if (!DueForLog(g_lastStuckLogMs)) return;
    EventLog::Write("INJECT: %ld px of cursor movement accepted but the cursor "
                    "has not moved from (%ld,%ld) — something is discarding "
                    "injected motion (cursor clip, or a low-level hook)",
                    travelPx, at.x, at.y);
    LogEnvironment("state while cursor is stuck");
}

}  // namespace InputInjection
