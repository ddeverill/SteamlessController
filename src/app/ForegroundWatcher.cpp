#include "ForegroundWatcher.h"
#include <appmodel.h>
#include <vector>

ForegroundWatcher* ForegroundWatcher::s_instance = nullptr;

namespace {

// Packaged apps do not own their own top-level window. Windows hosts it in
// ApplicationFrameHost.exe, so the foreground window resolves to the host and
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

ForegroundIdentity IdentifyProcess(DWORD pid) {
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

}  // namespace

ForegroundIdentity ForegroundWatcher::Current() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return {};

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    ForegroundIdentity id = IdentifyProcess(pid);

    if (!id.exePath.empty() && IsFrameHost(id.exePath)) {
        FrameChildSearch search{ pid, nullptr };
        EnumChildWindows(hwnd, FindFrameChild, reinterpret_cast<LPARAM>(&search));
        if (search.found) {
            DWORD childPid = 0;
            GetWindowThreadProcessId(search.found, &childPid);
            ForegroundIdentity hosted = IdentifyProcess(childPid);
            // Only take the hosted identity if it resolved; a frame with no
            // reachable child is still better described by the frame itself
            // than by nothing at all.
            if (!hosted.Empty()) id = hosted;
        }
    }

    return id;
}

void CALLBACK ForegroundWatcher::HookProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                          LONG idObject, LONG, DWORD, DWORD) {
    if (!s_instance || !s_instance->m_hwnd) return;
    if (event != EVENT_SYSTEM_FOREGROUND || idObject != OBJID_WINDOW || !hwnd) return;

    // Deliberately does not resolve anything here. The window that raised the
    // event is often not the one focus settles on, so the timer re-reads the
    // foreground when it expires and a burst collapses into one answer.
    SetTimer(s_instance->m_hwnd, IDT_DEBOUNCE, DEBOUNCE_MS, nullptr);
}

LRESULT CALLBACK ForegroundWatcher::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == IDT_DEBOUNCE && s_instance) {
        KillTimer(hwnd, IDT_DEBOUNCE);
        s_instance->OnDebounceElapsed();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ForegroundWatcher::OnDebounceElapsed() {
    const ForegroundIdentity now = Current();
    // An unidentifiable foreground is not a change worth reporting: it says
    // nothing about what the user switched to, and treating it as "no game"
    // would drop a profile every time focus passed through something we
    // cannot open.
    if (now.Empty() || now == m_last) return;
    m_last = now;
    if (m_onChange) m_onChange(now);
}

bool ForegroundWatcher::Start(ChangedFn onChange) {
    Stop();
    m_onChange = std::move(onChange);
    s_instance = this;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);  // OK if already registered

    // Message-only: it exists purely to own the debounce timer, and must
    // never appear anywhere a user could see it.
    m_hwnd = CreateWindowExW(0, CLASS_NAME, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!m_hwnd) {
        s_instance = nullptr;
        return false;
    }

    // SKIPOWNPROCESS: our own tray menu and settings window taking focus is
    // not the user switching applications, and would otherwise clear whatever
    // profile is running the moment they opened the menu.
    m_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                             nullptr, HookProc, 0, 0,
                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!m_hook) {
        DestroyWindow(m_hwnd);
        m_hwnd     = nullptr;
        s_instance = nullptr;
        return false;
    }

    m_last = Current();
    return true;
}

void ForegroundWatcher::Stop() {
    if (m_hook) {
        UnhookWinEvent(m_hook);
        m_hook = nullptr;
    }
    if (m_hwnd) {
        KillTimer(m_hwnd, IDT_DEBOUNCE);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (s_instance == this) s_instance = nullptr;
    m_onChange = nullptr;
    m_last     = {};
}
