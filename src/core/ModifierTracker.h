#pragma once
#include <cstddef>
#include <cstdint>
#include "KeyNames.h"

class IInputInjector;

// Presses/releases the modifiers of a Kind::Key binding around its base key,
// refcounted so overlapping bindings share a modifier correctly. Extracted
// unchanged (in behavior) from what used to be KeyInput.cpp's SendModifiers —
// this logic is platform-neutral policy, not I/O, so it is written once and
// shared by every IInputInjector backend.
//
// Two things make this more than a loop over IInputInjector::Key.
//
// A modifier the user is physically holding must be left alone: releasing
// Ctrl on a binding's behalf while the real Ctrl is still down tells every
// application the key came up, and the keyboard behaves oddly until the user
// taps it again.
//
// And two bindings can want the same modifier at once — paddles bound to
// Ctrl+C and Ctrl+V, pressed overlapping. Releasing the first must not drop
// the Ctrl the second is still relying on, so presses are counted and the
// key is released on the last one out.
class ModifierTracker {
public:
    explicit ModifierTracker(IInputInjector& injector) : m_injector(injector) {}

    void Apply(uint8_t mods, bool down);

private:
    static constexpr size_t kMaxModifiers = 8;  // headroom over the 4 currently defined

    IInputInjector& m_injector;
    int  m_holds[kMaxModifiers]      = {};
    bool m_sentByUs[kMaxModifiers]   = {};
};
