#pragma once
#include <chrono>
#include <cstdint>
#include <string>

enum class MouseButtonId { Left, Right, Middle, X1, X2 };

// Where the desktop cursor actually is, so TrackpadMouse's stuck-cursor
// diagnostic (sent movement accumulating without the cursor ever moving —
// a game's cursor clip, or a low-level hook silently eating injected input)
// can compare intent against reality. Wayland has no way to answer this at
// all — LinuxInputInjector::ProbeCursor always returns {available=false},
// which makes the diagnostic code that consumes it compile unchanged and
// simply never fire there, rather than needing an #ifdef in shared logic.
struct CursorProbe {
    bool available = false;
    long x = 0;
    long y = 0;
};

// Synthetic mouse/keyboard input for trackpad-as-mouse and key-bound paddles.
// Windows: SendInput. Linux: a pair of uinput virtual devices (mouse +
// keyboard) — see platform/linux/LinuxInputInjector.h for why that is the
// correct Wayland-safe mechanism instead of XTest or a portal.
class IInputInjector {
public:
    virtual ~IInputInjector() = default;

    // Returns whether the move was accepted for delivery (not whether the
    // cursor actually moved — see ProbeCursor for that).
    virtual bool MoveMouse(int dx, int dy) = 0;
    // vDelta/hDelta are in the traditional 120-units-per-notch convention on
    // both platforms; the Linux backend also emits the REL_WHEEL_HI_RES
    // sibling event for smooth-scroll compositors.
    virtual void Scroll(int vDelta, int hDelta) = 0;
    virtual void Button(MouseButtonId button, bool down) = 0;

    // keyId is the app's canonical key id — numerically equal to a Windows
    // virtual-key code (kept as the canonical id because existing profiles
    // and the `key:<n>` wire format already depend on it; see KeyNames.h).
    virtual void Key(uint16_t keyId, bool down) = 0;

    // Release every key/button this injector currently believes is held.
    // Called on shutdown so a bound key doesn't get stuck down.
    virtual void ReleaseAll() = 0;

    virtual std::string KeyDisplayName(uint16_t keyId) const = 0;

    // The user's own repeat-rate settings, so a held key auto-repeats the
    // way a physical keyboard would (injected input has no firmware behind
    // it to do this on its own).
    virtual std::chrono::milliseconds KeyRepeatDelay() const = 0;
    virtual std::chrono::milliseconds KeyRepeatInterval() const = 0;

    // Whether keyId is held right now by something other than this injector
    // (i.e. the user's own finger). Windows answers this with
    // GetAsyncKeyState; a Linux backend has no equivalent without grabbing
    // real input devices and always returns false — meaning a Linux modifier
    // release always fires, which is a documented, minor behavioral
    // difference (see ModifierTracker), not a bug to fix here.
    virtual bool IsPhysicallyHeld(uint16_t keyId) const = 0;

    virtual CursorProbe ProbeCursor() const = 0;
    // One-line diagnostic hooks used by ControllerManager/TrackpadMouse —
    // Windows logs UIPI/session details; Linux backends may no-op.
    virtual void LogEnvironment(const char* reason) = 0;
    virtual void LogCursorNotMoving(long travelPx, long x, long y) = 0;
};
