#pragma once
#include "core/iface/IOnScreenKeyboard.h"

// Delegates to the original TouchKeyboard::Toggle() (unchanged from before
// the port — see WinOnScreenKeyboard.h/.cpp). Unlike the Steam/game-library
// adapters, this one IS exercised: ControllerManager::SendPaddleInput calls
// IPlatform::Osk().Toggle() directly for the touchKeyboard binding.
class WinOnScreenKeyboardAdapter : public IOnScreenKeyboard {
public:
    void Toggle() override;
    bool Available() const override { return true; }  // a built-in OS feature on any Windows 10/11
};
