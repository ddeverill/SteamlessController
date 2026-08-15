#pragma once
#include "core/iface/IInputInjector.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <functional>
#include <mutex>
#include <string>

// Synthetic input, with the reasons it can fail recorded.
//
// Every key, mouse button and cursor move this app produces leaves through
// SendInput, and SendInput is the one stage of the pipeline Windows can refuse
// without any other symptom: haptics and the virtual pad are HID writes and
// keep working regardless. A refused injection therefore looks exactly like
// "the trackpad stopped moving the mouse, but the pad still buzzes", with
// nothing in the log to say why.
//
// The refusal that matters is UIPI: a process may not inject input while the
// foreground window belongs to a process at a higher integrity level, so an
// elevated game, launcher or tool in the foreground silently swallows
// everything this app sends until focus moves elsewhere or that window closes.
// This app never runs elevated (see DeviceRestart) precisely so that it is not
// the one doing the blocking, which leaves it on the receiving end.
//
// The refusal is not visible in SendInput's result: on Windows 11 the call
// reports success and the event is discarded afterwards, so the only reliable
// way to know is to ask, before injecting, whether the foreground window
// outranks this process.
class WinInputInjector : public IInputInjector {
public:
    // How the app tells the user something is wrong (the tray balloon).
    // Called from a controller read thread — the receiver must marshal to
    // the UI thread, same contract as ControllerManager::AlertFn.
    using AlertFn = std::function<void(const std::string& title, const std::string& text)>;
    void SetAlertCallback(AlertFn fn);

    bool MoveMouse(int dx, int dy) override;
    void Scroll(int vDelta, int hDelta) override;
    void Button(MouseButtonId button, bool down) override;
    void Key(uint16_t keyId, bool down) override;
    void ReleaseAll() override {}  // nothing tracked below ControllerManager's own held state

    std::string KeyDisplayName(uint16_t keyId) const override;
    std::chrono::milliseconds KeyRepeatDelay() const override;
    std::chrono::milliseconds KeyRepeatInterval() const override;
    bool IsPhysicallyHeld(uint16_t keyId) const override;

    CursorProbe ProbeCursor() const override;
    void LogEnvironment(const char* reason) override;
    void LogCursorNotMoving(long travelPx, long x, long y) override;

private:
    bool Send(const INPUT& input, const char* what);

    // Exe name of the foreground window's process when it outranks this one,
    // so anything injected now is discarded. Empty when injection can land.
    //
    // A process that cannot be opened at all — protected processes, in
    // practice anti-cheat — is deliberately NOT called blocked: its level is
    // unknown, and guessing would put a wrong name in front of the user. The
    // stuck-cursor check still catches it, and LogEnvironment records that
    // the process was unreadable.
    std::wstring ForegroundBlocker(DWORD& pidOut);

    // Records the blocked state changing: one log line per blocker rather
    // than per event, and a balloon the first time each blocker gets in the way.
    void NoteBlockState(const std::wstring& blocker, DWORD pid, const char* what);

    mutable std::mutex m_stateMutex;
    AlertFn      m_alertFn;
    std::wstring m_blockerLogged;  // blocker named in the log right now
    std::wstring m_alertBlocker;   // blocker the last balloon named
    uint64_t     m_lastAlertMs = 0;

    // Foreground lookups are cached against the window they describe:
    // injection happens at report rate, foreground changes do not, and a
    // process cannot change integrity level while it lives.
    HWND         m_cachedHwnd    = nullptr;
    DWORD        m_cachedPid     = 0;
    bool         m_cachedBlocked = false;
    std::wstring m_cachedName;
};
