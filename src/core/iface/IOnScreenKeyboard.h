#pragma once

// The touchKeyboard back-button action. Windows: opens/targets the built-in
// on-screen keyboard (osk.exe). Linux: best-effort exec of whichever virtual
// keyboard (wvkbd/onboard/squeekboard) is found on PATH — not a guaranteed
// feature there.
class IOnScreenKeyboard {
public:
    virtual ~IOnScreenKeyboard() = default;

    // A toggle, not a held input — see BackButtonAction::TouchKeyboard.
    virtual void Toggle() = 0;
    virtual bool Available() const = 0;
};
