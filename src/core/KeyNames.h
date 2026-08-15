#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "BackButtonConfig.h"

// The app's canonical key-id space. Numerically equal to the Windows VK_*
// constant of the same name — kept that way because BackButtonBinding::code
// (a Kind::Key binding), the packed-DWORD registry format, and the `key:<n>`
// wire format all already depend on these exact values from existing
// Windows installs. Spelled out here as plain constants (rather than
// including <Windows.h>) so this table stays platform-neutral; each
// platform's IInputInjector translates a canonical id to its own native key
// code (Windows: back to a VK, trivially; Linux: VK -> evdev KEY_*, see
// platform/linux/LinuxKeyMap.cpp).
namespace KeyId {
constexpr uint16_t Space  = 0x20, Return = 0x0D, Tab = 0x09, Back = 0x08;
constexpr uint16_t Delete = 0x2E, Insert = 0x2D;
constexpr uint16_t Left   = 0x25, Up = 0x26, Right = 0x27, Down = 0x28;
constexpr uint16_t Home   = 0x24, End = 0x23, Prior = 0x21, Next = 0x22;
constexpr uint16_t LShift = 0xA0, RShift = 0xA1;
constexpr uint16_t LControl = 0xA2, RControl = 0xA3;
constexpr uint16_t LMenu = 0xA4, RMenu = 0xA5;
constexpr uint16_t LWin = 0x5B, RWin = 0x5C;
constexpr uint16_t Shift = 0x10, Control = 0x11, Menu = 0x12;  // merged forms
constexpr uint16_t F1 = 0x70, F2 = 0x71, F3 = 0x72, F4 = 0x73, F5 = 0x74, F6 = 0x75;
constexpr uint16_t F7 = 0x76, F8 = 0x77, F9 = 0x78, F10 = 0x79, F11 = 0x7A, F12 = 0x7B;
constexpr uint16_t OemMinus = 0xBD, OemPlus = 0xBB, Oem4 = 0xDB, Oem6 = 0xDD;
constexpr uint16_t Oem5 = 0xDC, Oem1 = 0xBA, Oem7 = 0xDE, Oem3 = 0xC0;
constexpr uint16_t OemComma = 0xBC, OemPeriod = 0xBE, Oem2 = 0xBF;
constexpr uint16_t Numpad0 = 0x60, Numpad1 = 0x61, Numpad2 = 0x62, Numpad3 = 0x63, Numpad4 = 0x64;
constexpr uint16_t Numpad5 = 0x65, Numpad6 = 0x66, Numpad7 = 0x67, Numpad8 = 0x68, Numpad9 = 0x69;
constexpr uint16_t Add = 0x6B, Subtract = 0x6D, Multiply = 0x6A, Divide = 0x6F, Decimal = 0x6E;
constexpr uint16_t Capital = 0x14, NumLock = 0x90, ScrollLock = 0x91;
constexpr uint16_t Apps = 0x5D, Snapshot = 0x2C;
}  // namespace KeyId

// Maps a KeyboardEvent.code value ("KeyA", "Space", "ArrowUp") to a canonical
// key id. Returns 0 for anything unrecognised or not bindable. Shared by the
// Windows remap UI, the Linux CLI, and config-file parsing, so all three name
// keys identically.
uint16_t KeyIdFromJsCode(const std::string& jsCode);

// Reverse lookup for display/round-tripping. Returns an empty string for a
// key id with no entry in the catalog (e.g. one that only ever arrives via
// the legacy packed numeric form).
std::string JsCodeFromKeyId(uint16_t keyId);

// One modifier flag/name/key-id triple, in the fixed press/label order:
// Ctrl, Alt, Shift, Win — the order Windows itself uses when spelling out a
// shortcut, and the order modifiers are pressed in.
struct ModifierKeyInfo {
    BackButtonBinding::Modifier flag;
    uint16_t                    keyId;  // the left-hand key sent for this modifier
    const char*                 name;
};
const ModifierKeyInfo* ModifierKeyTable(size_t& countOut);
