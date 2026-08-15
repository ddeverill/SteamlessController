#pragma once
#include <cstdint>
#include <string>
#include "core/BackButtonConfig.h"

// Keyboard *display* support for the remap window's key picker.
//
// Capture happens in the remap window's WebView2 page rather than through a
// keyboard hook: the page is already focused while a row is listening, and a
// low-level hook is a heavy, AV-bait mechanism for a convenience feature.
// The page reports the physical key as a KeyboardEvent.code string, which
// this module turns into a virtual-key code.
//
// Actually *sending* a key (SendKeyInput/SendModifiers, as this file used to
// declare) now lives on WinInputInjector — see core/iface/IInputInjector.h
// and core/ModifierTracker.h, which is where ControllerManager's paddle
// dispatch reaches them from. This file keeps only the display-side half,
// which has no equivalent there and is still needed by the remap UI.

// Maps a KeyboardEvent.code value ("KeyA", "Space", "ArrowUp") to a Windows
// virtual-key code. Returns 0 for anything unrecognised or not bindable.
uint16_t VkFromJsCode(const std::string& jsCode);

// Human-readable name for a virtual-key code, from the active keyboard layout —
// so a non-US layout shows its own key names. Falls back to "Key" if the layout
// has no name for it.
std::wstring KeyDisplayName(uint16_t vk);

// Name for a whole key binding, modifiers included: "Ctrl + Alt + K". The
// modifier names are fixed rather than drawn from the layout, because that is
// how every application spells them in its own shortcut lists.
std::wstring KeyComboDisplayName(const BackButtonBinding& binding);
