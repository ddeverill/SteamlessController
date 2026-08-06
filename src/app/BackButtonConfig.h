#pragma once
#include <cstdint>
#include <string>

// Built-in actions a back paddle can be mapped to.
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

// A paddle binding: either one of the built-in actions above, or a value drawn
// from a space too large to enumerate (a keyboard virtual-key code, an extended
// mouse button).
//
// Persisted as a single DWORD, (kind << 16) | code. Every value written by
// builds that stored a bare BackButtonAction is 0-18, so its kind bits are
// already zero and it decodes as Kind::Action — old settings load unchanged
// with no migration step.
struct BackButtonBinding {
    enum class Kind : uint8_t {
        Action      = 0,  // code is a BackButtonAction
        Key         = 1,  // code is a Windows virtual-key code
        MouseButton = 2,  // code is a MouseButtonCode
    };

    // Extended mouse buttons. Left and right live in BackButtonAction instead,
    // where they were first persisted.
    enum class MouseButtonCode : uint16_t { Middle = 0, X1 = 1, X2 = 2, COUNT };

    Kind     kind = Kind::Action;
    uint16_t code = static_cast<uint16_t>(BackButtonAction::None);

    static BackButtonBinding FromAction(BackButtonAction a) {
        return { Kind::Action, static_cast<uint16_t>(a) };
    }
    static BackButtonBinding FromKey(uint16_t vk) {
        return { Kind::Key, vk };
    }
    static BackButtonBinding FromMouseButton(MouseButtonCode b) {
        return { Kind::MouseButton, static_cast<uint16_t>(b) };
    }

    bool IsAction() const { return kind == Kind::Action; }

    // The built-in action this binding names, or None when it is a key or an
    // extended mouse button. Callers that only understand actions can switch on
    // this directly — anything they cannot represent reads as "do nothing".
    BackButtonAction AsAction() const {
        return kind == Kind::Action ? static_cast<BackButtonAction>(code)
                                    : BackButtonAction::None;
    }
    bool IsAction(BackButtonAction a) const {
        return kind == Kind::Action && code == static_cast<uint16_t>(a);
    }

    bool operator==(const BackButtonBinding& o) const {
        return kind == o.kind && code == o.code;
    }
    bool operator!=(const BackButtonBinding& o) const { return !(*this == o); }

    uint32_t Pack() const {
        return (static_cast<uint32_t>(kind) << 16) | code;
    }

    // Anything unrecognised — a corrupt value, or one written by a future build
    // that knows kinds we do not — degrades to None rather than being applied.
    static BackButtonBinding Unpack(uint32_t packed) {
        const auto kind = static_cast<Kind>((packed >> 16) & 0xFF);
        const auto code = static_cast<uint16_t>(packed & 0xFFFF);
        switch (kind) {
        case Kind::Action:
            return code < static_cast<uint16_t>(BackButtonAction::COUNT)
                 ? BackButtonBinding{ Kind::Action, code }
                 : BackButtonBinding{};
        case Kind::Key:
            return code != 0 ? BackButtonBinding{ Kind::Key, code }
                             : BackButtonBinding{};
        case Kind::MouseButton:
            return code < static_cast<uint16_t>(MouseButtonCode::COUNT)
                 ? BackButtonBinding{ Kind::MouseButton, code }
                 : BackButtonBinding{};
        }
        return {};
    }

    // Wire format for the WebView2 UI. Built-in actions keep their bare ids
    // ("A", "leftMouse", "none"); the open-ended kinds are prefixed. Staying
    // with flat strings keeps the hand-rolled JSON reader in RemapWindow usable.
    std::string Id() const {
        switch (kind) {
        case Kind::Key:
            return "key:" + std::to_string(code);
        case Kind::MouseButton:
            switch (static_cast<MouseButtonCode>(code)) {
            case MouseButtonCode::Middle: return "mouse:middle";
            case MouseButtonCode::X1:     return "mouse:x1";
            case MouseButtonCode::X2:     return "mouse:x2";
            default:                      return "none";
            }
        case Kind::Action:
        default:
            return BackButtonActionId(AsAction());
        }
    }

    static BackButtonBinding FromId(const std::string& id) {
        if (id.rfind("key:", 0) == 0) {
            uint32_t vk = 0;
            for (size_t i = 4; i < id.size(); ++i) {
                if (id[i] < '0' || id[i] > '9') return {};
                vk = vk * 10 + static_cast<uint32_t>(id[i] - '0');
                if (vk > 0xFFFF) return {};
            }
            return vk != 0 ? FromKey(static_cast<uint16_t>(vk)) : BackButtonBinding{};
        }
        if (id == "mouse:middle") return FromMouseButton(MouseButtonCode::Middle);
        if (id == "mouse:x1")     return FromMouseButton(MouseButtonCode::X1);
        if (id == "mouse:x2")     return FromMouseButton(MouseButtonCode::X2);
        return FromAction(BackButtonActionFromId(id));
    }
};

struct BackButtonConfig {
    // Default: the upper paddles left-click the mouse (this used to be the
    // "Back Buttons as Mouse Click" tray toggle), the lower paddles are unbound.
    BackButtonBinding l4 = BackButtonBinding::FromAction(BackButtonAction::LeftMouseButton);
    BackButtonBinding l5 = BackButtonBinding::FromAction(BackButtonAction::None);
    BackButtonBinding r4 = BackButtonBinding::FromAction(BackButtonAction::LeftMouseButton);
    BackButtonBinding r5 = BackButtonBinding::FromAction(BackButtonAction::None);
};
