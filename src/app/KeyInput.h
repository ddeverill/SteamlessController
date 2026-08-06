#pragma once
#include <chrono>
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
//
// Holding a key down does not repeat on its own: auto-repeat comes from the
// keyboard's own typematic behaviour, not from Windows noticing a key is held,
// and injected input has no firmware behind it. A caller that wants a held key
// to repeat has to re-send the press itself, paced by the two values below.
void SendKeyInput(uint16_t vk, bool down);

// The user's Windows keyboard settings: how long a key must be held before it
// starts repeating, and the gap between repeats after that. Read fresh rather
// than cached at startup so a change in Control Panel is picked up.
std::chrono::milliseconds KeyRepeatDelay();
std::chrono::milliseconds KeyRepeatInterval();
