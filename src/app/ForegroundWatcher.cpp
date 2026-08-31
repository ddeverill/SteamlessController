#include "ForegroundWatcher.h"

ForegroundWatcher* ForegroundWatcher::s_instance = nullptr;

// The whole of resolving a window to an application now lives in
// ProcessIdentity, shared with the remap window's running-app picker.
ForegroundIdentity ForegroundWatcher::Current() {
    return ProcessIdentity::ForWindow(GetForegroundWindow());
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
