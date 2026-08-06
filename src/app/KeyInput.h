#pragma once
#include <cstdint>
#include <string>

// Keyboard support for paddle bindings.
//
// Capture happens in the remap window's WebView2 page rather than through a
// keyboard hook: the page is already focused while a row is listening, and a
// low-level hook is a heavy, AV-bait mechanism for a convenience feature.
// The page reports the physical key as a KeyboardEvent.code string, which this
// module turns into a virtual-key code.

// Maps a KeyboardEvent.code value ("KeyA", "Space", "ArrowUp") to a Windows
// virtual-key code. Returns 0 for anything unrecognised or not bindable.
uint16_t VkFromJsCode(const std::string& jsCode);

// Human-readable name for a virtual-key code, from the active keyboard layout —
// so a non-US layout shows its own key names. Falls back to "Key" if the layout
// has no name for it.
std::wstring KeyDisplayName(uint16_t vk);

// Presses or releases a key. Sends a scan code rather than a virtual key, and
// flags the extended keys, because software that reads scan codes directly
// (games, in particular) sees nothing useful otherwise.
void SendKeyInput(uint16_t vk, bool down);
