#pragma once
#include <sal.h>
#include <string>

// Persistent diagnostic event log for support and field debugging: device
// connect/disconnect causes, mode transitions, battery readings, stalls.
// Written to %LOCALAPPDATA%\SteamlessController\events.log and rotated to
// events.old.log at ~512 KB, so it can run forever without growing unbounded.
//
// Thread-safe. printf-style formatting; use %ls for wide strings (paths).
//
// %ls only survives non-ASCII because wWinMain sets LC_CTYPE to UTF-8 — see
// the note there. Without it fprintf abandons the line partway through rather
// than reporting an error.
namespace EventLog {

// Annotated so the compiler type-checks every format string against its
// arguments. These lines are read long after the fact, by whoever is trying to
// explain a fault they cannot reproduce; a specifier that does not match its
// argument corrupts the one record of what happened, and the paths that log
// least often — failures — are exactly the ones no test run exercises.
void Write(_Printf_format_string_ const char* fmt, ...);

// Full path of the current log file (for "Open Event Log" in the tray menu).
std::wstring FilePath();

}
