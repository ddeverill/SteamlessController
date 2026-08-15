#include "LinuxKeyMap.h"
#include "core/KeyNames.h"
#include <linux/input-event-codes.h>

uint16_t EvdevKeyFromKeyId(uint16_t keyId) {
    // Letters and digits carry their ASCII value as the canonical key id
    // (see core/KeyNames.cpp); evdev's KEY_* codes are not a simple offset
    // from ASCII, so each needs its own entry.
    switch (keyId) {
    case 'A': return KEY_A; case 'B': return KEY_B; case 'C': return KEY_C;
    case 'D': return KEY_D; case 'E': return KEY_E; case 'F': return KEY_F;
    case 'G': return KEY_G; case 'H': return KEY_H; case 'I': return KEY_I;
    case 'J': return KEY_J; case 'K': return KEY_K; case 'L': return KEY_L;
    case 'M': return KEY_M; case 'N': return KEY_N; case 'O': return KEY_O;
    case 'P': return KEY_P; case 'Q': return KEY_Q; case 'R': return KEY_R;
    case 'S': return KEY_S; case 'T': return KEY_T; case 'U': return KEY_U;
    case 'V': return KEY_V; case 'W': return KEY_W; case 'X': return KEY_X;
    case 'Y': return KEY_Y; case 'Z': return KEY_Z;
    case '0': return KEY_0; case '1': return KEY_1; case '2': return KEY_2;
    case '3': return KEY_3; case '4': return KEY_4; case '5': return KEY_5;
    case '6': return KEY_6; case '7': return KEY_7; case '8': return KEY_8;
    case '9': return KEY_9;

    case KeyId::Space:  return KEY_SPACE;
    case KeyId::Return: return KEY_ENTER;
    case KeyId::Tab:    return KEY_TAB;
    case KeyId::Back:   return KEY_BACKSPACE;
    case KeyId::Delete: return KEY_DELETE;
    case KeyId::Insert: return KEY_INSERT;

    case KeyId::Left:  return KEY_LEFT;
    case KeyId::Up:    return KEY_UP;
    case KeyId::Right: return KEY_RIGHT;
    case KeyId::Down:  return KEY_DOWN;
    case KeyId::Home:  return KEY_HOME;
    case KeyId::End:   return KEY_END;
    case KeyId::Prior: return KEY_PAGEUP;
    case KeyId::Next:  return KEY_PAGEDOWN;

    case KeyId::LShift:   return KEY_LEFTSHIFT;
    case KeyId::RShift:   return KEY_RIGHTSHIFT;
    case KeyId::LControl: return KEY_LEFTCTRL;
    case KeyId::RControl: return KEY_RIGHTCTRL;
    case KeyId::LMenu:    return KEY_LEFTALT;
    case KeyId::RMenu:    return KEY_RIGHTALT;
    case KeyId::LWin:     return KEY_LEFTMETA;
    case KeyId::RWin:     return KEY_RIGHTMETA;

    case KeyId::F1: return KEY_F1;   case KeyId::F2:  return KEY_F2;
    case KeyId::F3: return KEY_F3;   case KeyId::F4:  return KEY_F4;
    case KeyId::F5: return KEY_F5;   case KeyId::F6:  return KEY_F6;
    case KeyId::F7: return KEY_F7;   case KeyId::F8:  return KEY_F8;
    case KeyId::F9: return KEY_F9;   case KeyId::F10: return KEY_F10;
    case KeyId::F11: return KEY_F11; case KeyId::F12: return KEY_F12;

    case KeyId::OemMinus: return KEY_MINUS;
    case KeyId::OemPlus:  return KEY_EQUAL;
    case KeyId::Oem4:     return KEY_LEFTBRACE;
    case KeyId::Oem6:     return KEY_RIGHTBRACE;
    case KeyId::Oem5:     return KEY_BACKSLASH;
    case KeyId::Oem1:     return KEY_SEMICOLON;
    case KeyId::Oem7:     return KEY_APOSTROPHE;
    case KeyId::Oem3:     return KEY_GRAVE;
    case KeyId::OemComma: return KEY_COMMA;
    case KeyId::OemPeriod:return KEY_DOT;
    case KeyId::Oem2:     return KEY_SLASH;

    case KeyId::Numpad0: return KEY_KP0; case KeyId::Numpad1: return KEY_KP1;
    case KeyId::Numpad2: return KEY_KP2; case KeyId::Numpad3: return KEY_KP3;
    case KeyId::Numpad4: return KEY_KP4; case KeyId::Numpad5: return KEY_KP5;
    case KeyId::Numpad6: return KEY_KP6; case KeyId::Numpad7: return KEY_KP7;
    case KeyId::Numpad8: return KEY_KP8; case KeyId::Numpad9: return KEY_KP9;
    case KeyId::Add:      return KEY_KPPLUS;
    case KeyId::Subtract: return KEY_KPMINUS;
    case KeyId::Multiply: return KEY_KPASTERISK;
    case KeyId::Divide:   return KEY_KPSLASH;
    case KeyId::Decimal:  return KEY_KPDOT;

    case KeyId::Capital:    return KEY_CAPSLOCK;
    case KeyId::NumLock:    return KEY_NUMLOCK;
    case KeyId::ScrollLock: return KEY_SCROLLLOCK;

    default: return 0;  // no clean evdev counterpart (e.g. layout-dependent VK_OEM_* not covered above)
    }
}
