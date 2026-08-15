#include "KeyNames.h"
#include <cstddef>

namespace {

struct JsCodeToKeyId { const char* code; uint16_t keyId; };

// KeyboardEvent.code values are physical positions, so a binding made on one
// keyboard layout means the same physical key on another. Escape is absent on
// purpose — the remap window uses it to cancel listening.
const JsCodeToKeyId kCodeMap[] = {
    // Letters
    {"KeyA",'A'},{"KeyB",'B'},{"KeyC",'C'},{"KeyD",'D'},{"KeyE",'E'},{"KeyF",'F'},
    {"KeyG",'G'},{"KeyH",'H'},{"KeyI",'I'},{"KeyJ",'J'},{"KeyK",'K'},{"KeyL",'L'},
    {"KeyM",'M'},{"KeyN",'N'},{"KeyO",'O'},{"KeyP",'P'},{"KeyQ",'Q'},{"KeyR",'R'},
    {"KeyS",'S'},{"KeyT",'T'},{"KeyU",'U'},{"KeyV",'V'},{"KeyW",'W'},{"KeyX",'X'},
    {"KeyY",'Y'},{"KeyZ",'Z'},

    // Digit row
    {"Digit0",'0'},{"Digit1",'1'},{"Digit2",'2'},{"Digit3",'3'},{"Digit4",'4'},
    {"Digit5",'5'},{"Digit6",'6'},{"Digit7",'7'},{"Digit8",'8'},{"Digit9",'9'},

    // Whitespace and editing
    {"Space",       KeyId::Space},
    {"Enter",       KeyId::Return},
    {"Tab",         KeyId::Tab},
    {"Backspace",   KeyId::Back},
    {"Delete",      KeyId::Delete},
    {"Insert",      KeyId::Insert},

    // Navigation
    {"ArrowUp",     KeyId::Up},
    {"ArrowDown",   KeyId::Down},
    {"ArrowLeft",   KeyId::Left},
    {"ArrowRight",  KeyId::Right},
    {"Home",        KeyId::Home},
    {"End",         KeyId::End},
    {"PageUp",      KeyId::Prior},
    {"PageDown",    KeyId::Next},

    // Modifiers — bound to the specific left/right key, not the merged form
    {"ShiftLeft",    KeyId::LShift},   {"ShiftRight",   KeyId::RShift},
    {"ControlLeft",  KeyId::LControl}, {"ControlRight", KeyId::RControl},
    {"AltLeft",      KeyId::LMenu},    {"AltRight",     KeyId::RMenu},
    // Present for completeness, not because they can be captured: Windows
    // opens the Start menu on the way down, so the page is never told. A
    // binding that wants Windows carries it as a modifier flag instead.
    {"MetaLeft",     KeyId::LWin},     {"MetaRight",    KeyId::RWin},

    // Function row
    {"F1",KeyId::F1},{"F2",KeyId::F2},{"F3",KeyId::F3},{"F4",KeyId::F4},
    {"F5",KeyId::F5},{"F6",KeyId::F6},{"F7",KeyId::F7},{"F8",KeyId::F8},
    {"F9",KeyId::F9},{"F10",KeyId::F10},{"F11",KeyId::F11},{"F12",KeyId::F12},

    // Punctuation (OEM codes are positional, which suits a physical binding)
    {"Minus",        KeyId::OemMinus},
    {"Equal",        KeyId::OemPlus},
    {"BracketLeft",  KeyId::Oem4},
    {"BracketRight", KeyId::Oem6},
    {"Backslash",    KeyId::Oem5},
    {"Semicolon",    KeyId::Oem1},
    {"Quote",        KeyId::Oem7},
    {"Backquote",    KeyId::Oem3},
    {"Comma",        KeyId::OemComma},
    {"Period",       KeyId::OemPeriod},
    {"Slash",        KeyId::Oem2},

    // Numpad
    {"Numpad0",KeyId::Numpad0},{"Numpad1",KeyId::Numpad1},{"Numpad2",KeyId::Numpad2},
    {"Numpad3",KeyId::Numpad3},{"Numpad4",KeyId::Numpad4},{"Numpad5",KeyId::Numpad5},
    {"Numpad6",KeyId::Numpad6},{"Numpad7",KeyId::Numpad7},{"Numpad8",KeyId::Numpad8},
    {"Numpad9",KeyId::Numpad9},
    {"NumpadAdd",      KeyId::Add},
    {"NumpadSubtract", KeyId::Subtract},
    {"NumpadMultiply", KeyId::Multiply},
    {"NumpadDivide",   KeyId::Divide},
    {"NumpadDecimal",  KeyId::Decimal},
    // Windows gives numpad Enter the same virtual key as the main one and
    // separates them by scan code alone. A binding stores only the key id, so
    // this one behaves as a plain Enter — which is what nearly all software wants.
    {"NumpadEnter",    KeyId::Return},

    // Locks
    {"CapsLock",   KeyId::Capital},
    {"NumLock",    KeyId::NumLock},
    {"ScrollLock", KeyId::ScrollLock},
};

// Order matters twice over: it is the order modifiers are pressed in, and the
// order they read in a label. Ctrl, Alt, Shift, Win is the convention Windows
// itself uses when it spells out a shortcut.
const ModifierKeyInfo kModifierKeys[] = {
    { BackButtonBinding::ModCtrl,  KeyId::LControl, "Ctrl"  },
    { BackButtonBinding::ModAlt,   KeyId::LMenu,    "Alt"   },
    { BackButtonBinding::ModShift, KeyId::LShift,   "Shift" },
    { BackButtonBinding::ModWin,   KeyId::LWin,     "Win"   },
};

}  // namespace

uint16_t KeyIdFromJsCode(const std::string& jsCode) {
    for (const auto& e : kCodeMap)
        if (jsCode == e.code) return e.keyId;
    return 0;
}

std::string JsCodeFromKeyId(uint16_t keyId) {
    for (const auto& e : kCodeMap)
        if (e.keyId == keyId) return e.code;
    return {};
}

const ModifierKeyInfo* ModifierKeyTable(size_t& countOut) {
    countOut = std::size(kModifierKeys);
    return kModifierKeys;
}
