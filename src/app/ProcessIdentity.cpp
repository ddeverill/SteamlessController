#include "ProcessIdentity.h"
#include <appmodel.h>
#include <vector>

namespace {

// Packaged apps do not own their own top-level window. Windows hosts it in
// ApplicationFrameHost.exe, so a foreground window resolves to the host and
// every Store app would look like the same application. The app's real window
// is a child of the frame, owned by a different process — that one carries
// the identity worth having.
constexpr wchar_t kFrameHostExe[] = L"ApplicationFrameHost.exe";

bool IsFrameHost(const std::wstring& exePath) {
    const size_t slash = exePath.find_last_of(L'\\');
    const wchar_t* leaf = slash == std::wstring::npos ? exePath.c_str()
                                                      : exePath.c_str() + slash + 1;
    return _wcsicmp(leaf, kFrameHostExe) == 0;
}

struct FrameChildSearch {
    DWORD hostPid = 0;
    HWND  found   = nullptr;
};

BOOL CALLBACK FindFrameChild(HWND child, LPARAM param) {
    auto* search = reinterpret_cast<FrameChildSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(child, &pid);
    if (pid != 0 && pid != search->hostPid) {
        search->found = child;
        return FALSE;  // stop at the first child that is not the host itself
    }
    return TRUE;
}

std::wstring ImagePathOf(HANDLE process) {
    // Longer than MAX_PATH on purpose: a packaged app's path under
    // WindowsApps carries a versioned package name and overruns it easily.
    wchar_t buf[1024];
    DWORD   len = ARRAYSIZE(buf);
    if (!QueryFullProcessImageNameW(process, 0, buf, &len)) return {};
    return std::wstring(buf, len);
}

std::wstring AumidOf(HANDLE process) {
    UINT32 len = 0;
    // Asking with no buffer is how the length comes back; anything other than
    // "too small" means this process has no package identity, which is the
    // ordinary case for a desktop application.
    if (GetApplicationUserModelId(process, &len, nullptr) != ERROR_INSUFFICIENT_BUFFER
        || len == 0)
        return {};

    std::vector<wchar_t> buf(len);
    if (GetApplicationUserModelId(process, &len, buf.data()) != ERROR_SUCCESS)
        return {};
    return std::wstring(buf.data());  // len counts the terminator; trust the string
}

}  // namespace

namespace ProcessIdentity {

ForegroundIdentity ForProcess(DWORD pid) {
    ForegroundIdentity id;
    if (pid == 0) return id;

    // Limited information is enough for both queries and, unlike the fuller
    // rights, is granted to an unelevated process for most things the user
    // is likely to be looking at.
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return id;  // protected or elevated — reported as unknown

    id.exePath = ImagePathOf(proc);
    id.aumid   = AumidOf(proc);
    CloseHandle(proc);
    return id;
}

ForegroundIdentity ForWindow(HWND hwnd) {
    if (!hwnd) return {};

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    ForegroundIdentity id = ForProcess(pid);

    if (!id.exePath.empty() && IsFrameHost(id.exePath)) {
        FrameChildSearch search{ pid, nullptr };
        EnumChildWindows(hwnd, FindFrameChild, reinterpret_cast<LPARAM>(&search));
        if (search.found) {
            DWORD childPid = 0;
            GetWindowThreadProcessId(search.found, &childPid);
            ForegroundIdentity hosted = ForProcess(childPid);
            // Only take the hosted identity if it resolved; a frame with no
            // reachable child is still better described by the frame itself
            // than by nothing at all.
            if (!hosted.Empty()) id = hosted;
        }
    }

    return id;
}

}  // namespace ProcessIdentity
