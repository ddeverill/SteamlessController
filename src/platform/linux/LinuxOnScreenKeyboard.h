#pragma once
#include "core/iface/IOnScreenKeyboard.h"

// Best-effort only, unlike the Windows on-screen keyboard binding: there is
// no universal Linux on-screen keyboard command, so this shells out to
// whichever of a few common virtual keyboards is on PATH. Toggle() logs
// once and does nothing when none is found — not a v1 blocker for the port.
class LinuxOnScreenKeyboard : public IOnScreenKeyboard {
public:
    void Toggle() override;
    bool Available() const override;
};
