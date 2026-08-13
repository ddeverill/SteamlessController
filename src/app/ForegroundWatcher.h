#pragma once
#include <Windows.h>
#include <functional>
#include <string>

// Who owns the window the user is currently working in.
//
// Both fields are filled where they apply: every process has an image path,
// and only a packaged (MSIX/UWP) one has an AUMID. A foreground window we
// could not identify at all — a protected process, or one that closed while
// being asked — reports empty, which callers should read as "unknown", never
// as "the desktop".
struct ForegroundIdentity {
    std::wstring exePath;
    std::wstring aumid;

    bool Empty() const { return exePath.empty() && aumid.empty(); }
    bool operator==(const ForegroundIdentity& o) const {
        return _wcsicmp(exePath.c_str(), o.exePath.c_str()) == 0
            && _wcsicmp(aumid.c_str(),   o.aumid.c_str())   == 0;
    }
    bool operator!=(const ForegroundIdentity& o) const { return !(*this == o); }
};

// Reports which application the user has switched to, without polling.
//
// Built on SetWinEventHook(EVENT_SYSTEM_FOREGROUND), which is the only
// push-based answer to "what is the user doing now" available to a process
// that never runs elevated: the event-driven alternatives for watching
// processes start (Win32_ProcessStartTrace, an ETW kernel session) all
// require administrator rights, and the unelevated WMI route polls
// internally. Registering costs one hook and no thread; the callback only
// runs when focus actually changes, which is a human-scale rate.
//
// Focus is also the better question than "did a game launch". A launcher
// that starts a game and stays resident — Ubisoft Connect, the EA app —
// keeps its process alive long after the game exits, so "is it running"
// answers yes for far too long. What is in front does not have that problem,
// and it means alt-tabbing to a browser hands the desktop its own settings
// back.
class ForegroundWatcher {
public:
    using ChangedFn = std::function<void(const ForegroundIdentity&)>;

    ~ForegroundWatcher() { Stop(); }

    // Begins reporting. The callback runs on the calling thread, which must
    // be the one pumping messages — out-of-context hooks are delivered
    // through the message queue, and the debounce below is a window timer.
    bool Start(ChangedFn onChange);
    void Stop();

    // Resolve the foreground window right now, for callers that need to
    // catch up rather than wait for the next switch (taking the controller
    // back, say, when the user has been sitting in a game the whole time).
    static ForegroundIdentity Current();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void CALLBACK HookProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                  LONG idObject, LONG idChild,
                                  DWORD thread, DWORD time);
    void OnDebounceElapsed();

    HWINEVENTHOOK m_hook   = nullptr;
    HWND          m_hwnd   = nullptr;  // message-only, owns the debounce timer
    ChangedFn     m_onChange;
    ForegroundIdentity m_last;

    static ForegroundWatcher* s_instance;

    static constexpr wchar_t CLASS_NAME[] = L"SteamlessForegroundWatcher";
    static constexpr UINT_PTR IDT_DEBOUNCE = 1;
    // Focus churns while an application starts — splash screens, launcher
    // windows, the game's own window replacing its loader — and every one of
    // those is a foreground change. Settling first keeps a burst from being
    // read as several different applications, which matters because a
    // profile switch can rebuild the virtual controller.
    static constexpr UINT DEBOUNCE_MS = 400;
};
