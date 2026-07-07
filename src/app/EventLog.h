#pragma once
#include <string>

// Persistent diagnostic event log for support and field debugging: device
// connect/disconnect causes, mode transitions, battery readings, stalls.
// Written to %LOCALAPPDATA%\SteamlessController\events.log and rotated to
// events.old.log at ~512 KB, so it can run forever without growing unbounded.
//
// Thread-safe. printf-style formatting; use %ls for wide strings (paths).
namespace EventLog {

void Write(const char* fmt, ...);

// Full path of the current log file (for "Open Event Log" in the tray menu).
std::wstring FilePath();

}
