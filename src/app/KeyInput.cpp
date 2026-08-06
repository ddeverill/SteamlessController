#include "KeyInput.h"
#include <Windows.h>
#include <iterator>

namespace {

struct JsCodeToVk { const char* code; uint16_t vk; };

// KeyboardEvent.code values are physical positions, so a binding made on one
// keyboard layout means the same physical key on another. Escape is absent on
// purpose — the remap window uses it to cancel listening.
const JsCodeToVk kCodeMap[] = {
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
    {"Space",       VK_SPACE},
    {"Enter",       VK_RETURN},
    {"Tab",         VK_TAB},
    {"Backspace",   VK_BACK},
    {"Delete",      VK_DELETE},
    {"Insert",      VK_INSERT},

    // Navigation
    {"ArrowUp",     VK_UP},
    {"ArrowDown",   VK_DOWN},
    {"ArrowLeft",   VK_LEFT},
    {"ArrowRight",  VK_RIGHT},
    {"Home",        VK_HOME},
    {"End",         VK_END},
    {"PageUp",      VK_PRIOR},
    {"PageDown",    VK_NEXT},

    // Modifiers — bound to the specific left/right key, not the merged VK_SHIFT
    {"ShiftLeft",    VK_LSHIFT},   {"ShiftRight",   VK_RSHIFT},
    {"ControlLeft",  VK_LCONTROL}, {"ControlRight", VK_RCONTROL},
    {"AltLeft",      VK_LMENU},    {"AltRight",     VK_RMENU},

    // Function row
    {"F1",VK_F1},{"F2",VK_F2},{"F3",VK_F3},{"F4",VK_F4},{"F5",VK_F5},{"F6",VK_F6},
    {"F7",VK_F7},{"F8",VK_F8},{"F9",VK_F9},{"F10",VK_F10},{"F11",VK_F11},{"F12",VK_F12},

    // Punctuation (OEM codes are positional, which suits a physical binding)
    {"Minus",        VK_OEM_MINUS},
    {"Equal",        VK_OEM_PLUS},
    {"BracketLeft",  VK_OEM_4},
    {"BracketRight", VK_OEM_6},
    {"Backslash",    VK_OEM_5},
    {"Semicolon",    VK_OEM_1},
    {"Quote",        VK_OEM_7},
    {"Backquote",    VK_OEM_3},
    {"Comma",        VK_OEM_COMMA},
    {"Period",       VK_OEM_PERIOD},
    {"Slash",        VK_OEM_2},

    // Numpad
    {"Numpad0",VK_NUMPAD0},{"Numpad1",VK_NUMPAD1},{"Numpad2",VK_NUMPAD2},
    {"Numpad3",VK_NUMPAD3},{"Numpad4",VK_NUMPAD4},{"Numpad5",VK_NUMPAD5},
    {"Numpad6",VK_NUMPAD6},{"Numpad7",VK_NUMPAD7},{"Numpad8",VK_NUMPAD8},
    {"Numpad9",VK_NUMPAD9},
    {"NumpadAdd",      VK_ADD},
    {"NumpadSubtract", VK_SUBTRACT},
    {"NumpadMultiply", VK_MULTIPLY},
    {"NumpadDivide",   VK_DIVIDE},
    {"NumpadDecimal",  VK_DECIMAL},
    // Windows gives numpad Enter the same virtual key as the main one and
    // separates them by scan code alone. A binding stores only the VK, so this
    // one behaves as a plain Enter — which is what nearly all software wants.
    {"NumpadEnter",    VK_RETURN},

    // Locks
    {"CapsLock",   VK_CAPITAL},
    {"NumLock",    VK_NUMLOCK},
    {"ScrollLock", VK_SCROLL},
};

// The navigation cluster shares scan codes with the numpad and is told apart
// only by the 0xE0 extended prefix — drop it and an Up arrow arrives as numpad
// 8. MapVirtualKey is documented to report that prefix under MAPVK_VK_TO_VSC_EX
// but does not actually do so for these keys, so the set is spelled out here.
bool IsExtendedKey(uint16_t vk) {
    switch (vk) {
    case VK_RCONTROL: case VK_RMENU:
    case VK_INSERT:   case VK_DELETE:
    case VK_HOME:     case VK_END:
    case VK_PRIOR:    case VK_NEXT:
    case VK_UP:       case VK_DOWN:
    case VK_LEFT:     case VK_RIGHT:
    case VK_NUMLOCK:  case VK_DIVIDE:
    case VK_SNAPSHOT:
    case VK_LWIN:     case VK_RWIN:  case VK_APPS:
        return true;
    default:
        return false;
    }
}

struct ScanCode { WORD scan; bool extended; };

ScanCode ScanCodeFor(uint16_t vk) {
    // _EX is still the right lookup for the scan code itself: it distinguishes
    // left from right modifiers, which the plain mapping collapses.
    const UINT mapped = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    return { static_cast<WORD>(mapped & 0xFF), IsExtendedKey(vk) };
}

}  // namespace

uint16_t VkFromJsCode(const std::string& jsCode) {
    for (const auto& e : kCodeMap)
        if (jsCode == e.code) return e.vk;
    return 0;
}

std::wstring KeyDisplayName(uint16_t vk) {
    if (vk == 0) return L"Key";

    const ScanCode sc = ScanCodeFor(vk);
    if (sc.scan == 0) return L"Key";

    // GetKeyNameText takes the scan code and extended flag positioned as they
    // appear in a WM_KEYDOWN lParam.
    LONG lParam = static_cast<LONG>(sc.scan) << 16;
    if (sc.extended) lParam |= (1L << 24);

    wchar_t buf[64] = {};
    const int n = GetKeyNameTextW(lParam, buf, static_cast<int>(std::size(buf)));
    return n > 0 ? std::wstring(buf, static_cast<size_t>(n)) : std::wstring(L"Key");
}

void SendKeyInput(uint16_t vk, bool down) {
    if (vk == 0) return;

    const ScanCode sc = ScanCodeFor(vk);
    if (sc.scan == 0) return;

    INPUT input = {};
    input.type       = INPUT_KEYBOARD;
    input.ki.wVk     = 0;  // ignored, and must be zero, when sending a scan code
    input.ki.wScan   = sc.scan;
    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (sc.extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!down)       input.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

std::chrono::milliseconds KeyRepeatDelay() {
    // 0-3, meaning roughly 250ms to 1000ms in 250ms steps.
    DWORD setting = 1;
    if (!SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &setting, 0) || setting > 3)
        setting = 1;
    return std::chrono::milliseconds(250 * (static_cast<int>(setting) + 1));
}

std::chrono::milliseconds KeyRepeatInterval() {
    // 0-31, meaning roughly 2.5 to 30 repeats per second. Windows documents only
    // the endpoints, so interpolate the rate between them.
    DWORD setting = 31;
    if (!SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &setting, 0) || setting > 31)
        setting = 31;
    const double perSecond = 2.5 + (30.0 - 2.5) * (static_cast<double>(setting) / 31.0);
    return std::chrono::milliseconds(static_cast<long long>(1000.0 / perSecond + 0.5));
}
