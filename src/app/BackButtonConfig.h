#pragma once
#include <cstdint>
#include <string>

// Actions a back paddle can be mapped to.
// Values are stable — persisted as DWORDs in the registry.
// Entries 0-11 match the original ordering so existing settings are preserved.
enum class BackButtonAction : uint8_t {
    // Row 1 — face buttons
    A = 0, B, X, Y,
    // Row 2 — bumpers / triggers (Steam nomenclature: L1, L2, R1, R2)
    LB, RB, LT, RT,
    // Row 3 — d-pad
    DPadUp, DPadDown, DPadLeft, DPadRight,
    // Row 4 — stick clicks, menu buttons, mouse buttons, and "none" (paddle does nothing)
    LeftMouseButton, RightMouseButton,
    None,
    Menu, View,
    L3, R3,
    COUNT
};

struct BackButtonConfig {
    // Default: the upper paddles left-click the mouse (this used to be the
    // "Back Buttons as Mouse Click" tray toggle), the lower paddles are unbound.
    BackButtonAction l4 = BackButtonAction::LeftMouseButton;
    BackButtonAction l5 = BackButtonAction::None;
    BackButtonAction r4 = BackButtonAction::LeftMouseButton;
    BackButtonAction r5 = BackButtonAction::None;
};

// String IDs match the JS input catalog (used in JSON messages to/from WebView2).
inline const char* BackButtonActionId(BackButtonAction a) {
    static const char* ids[] = {
        "A","B","X","Y",
        "LB","RB","LT","RT",
        "Up","Down","Left","Right",
        "leftMouse","rightMouse",
        "none","menu","view","L3","R3"
    };
    auto i = static_cast<size_t>(a);
    return i < static_cast<size_t>(BackButtonAction::COUNT) ? ids[i] : "none";
}

inline BackButtonAction BackButtonActionFromId(const std::string& id) {
    if (id == "A")          return BackButtonAction::A;
    if (id == "B")          return BackButtonAction::B;
    if (id == "X")          return BackButtonAction::X;
    if (id == "Y")          return BackButtonAction::Y;
    if (id == "LB")         return BackButtonAction::LB;
    if (id == "RB")         return BackButtonAction::RB;
    if (id == "LT")         return BackButtonAction::LT;
    if (id == "RT")         return BackButtonAction::RT;
    if (id == "Up")         return BackButtonAction::DPadUp;
    if (id == "Down")       return BackButtonAction::DPadDown;
    if (id == "Left")       return BackButtonAction::DPadLeft;
    if (id == "Right")      return BackButtonAction::DPadRight;
    if (id == "leftMouse")  return BackButtonAction::LeftMouseButton;
    if (id == "rightMouse") return BackButtonAction::RightMouseButton;
    if (id == "menu")       return BackButtonAction::Menu;
    if (id == "view")       return BackButtonAction::View;
    if (id == "L3")         return BackButtonAction::L3;
    if (id == "R3")         return BackButtonAction::R3;
    return BackButtonAction::None;
}
