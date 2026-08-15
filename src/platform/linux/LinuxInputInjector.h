#pragma once
#include "core/iface/IInputInjector.h"
#include <cstdint>

// Trackpad-as-mouse and key-bound paddles on Wayland, via two lazily-created
// uinput virtual devices (a mouse and a keyboard) rather than XTest or a
// desktop-portal RemoteDesktop session.
//
// uinput creates a genuine kernel input device, which libinput opens via
// logind exactly as it would a real USB mouse — the compositor cannot tell
// the difference and does not try to, since this operates a layer below
// Wayland's client-to-client input-injection restriction rather than
// crossing it. The portal alternative was considered and rejected: it needs
// a one-time-per-session user consent dialog and cannot run unattended in a
// background daemon.
//
// Kept as two separate devices, not one combined mouse+keyboard: udev's
// input_id classifies a device with both REL_X and KEY_A bits as both a
// mouse and a keyboard, which confuses libinput's per-device-type handling
// and some compositors' input settings UI. Real hardware doesn't combine
// them either.
class LinuxInputInjector : public IInputInjector {
public:
    LinuxInputInjector() = default;
    ~LinuxInputInjector() override;
    LinuxInputInjector(const LinuxInputInjector&) = delete;
    LinuxInputInjector& operator=(const LinuxInputInjector&) = delete;

    bool MoveMouse(int dx, int dy) override;
    void Scroll(int vDelta, int hDelta) override;
    void Button(MouseButtonId button, bool down) override;
    void Key(uint16_t keyId, bool down) override;
    void ReleaseAll() override;

    std::string KeyDisplayName(uint16_t keyId) const override;
    std::chrono::milliseconds KeyRepeatDelay() const override;
    std::chrono::milliseconds KeyRepeatInterval() const override;
    // No way to distinguish "the user is physically holding this key" from
    // "nothing is holding it" without grabbing real input devices, which
    // this class deliberately doesn't do (that would fight every other
    // program's access to the keyboard). Always false — meaning a modifier
    // release on Linux always fires, a minor, documented divergence from
    // Windows' GetAsyncKeyState-based behavior, not a bug to fix here.
    bool IsPhysicallyHeld(uint16_t /*keyId*/) const override { return false; }

    // Wayland has no way to answer this at all; always reports unavailable,
    // which makes TrackpadMouse's stuck-cursor diagnostic compile unchanged
    // and simply never fire on this backend.
    CursorProbe ProbeCursor() const override { return {}; }
    void LogEnvironment(const char* reason) override;
    void LogCursorNotMoving(long travelPx, long x, long y) override;

private:
    bool EnsureMouse();
    bool EnsureKeyboard();
    static int CreateMouseDevice();
    static int CreateKeyboardDevice();

    int m_mouseFd    = -1;
    int m_keyboardFd = -1;

    // Sub-notch scroll carry, same role as TrackpadMouse's own remainder
    // carry but at the hi-res-vs-legacy-wheel-event boundary instead of the
    // pixel boundary.
    int m_scrollAccumV = 0;
    int m_scrollAccumH = 0;
};
