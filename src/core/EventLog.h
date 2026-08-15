#pragma once
#include "Format.h"
#include <string>

// Persistent diagnostic event log for support and field debugging: device
// connect/disconnect causes, mode transitions, battery readings, stalls.
// Written to a state directory rotated to events.old.log at ~512 KB, so it
// can run forever without growing unbounded.
//
// Thread-safe. printf-style formatting, UTF-8 throughout.
namespace EventLog {

// Sets the directory Write() logs into (created on first use). Call once at
// startup — IPlatformPaths::StateDir() on both platforms. Safe to call
// before any Write(); Write() before Init() silently does nothing.
void Init(const std::string& dir);

// Annotated so the compiler type-checks every format string against its
// arguments. These lines are read long after the fact, by whoever is trying to
// explain a fault they cannot reproduce; a specifier that does not match its
// argument corrupts the one record of what happened, and the paths that log
// least often — failures — are exactly the ones no test run exercises.
void Write(SC_PRINTF_FORMAT const char* fmt, ...) SC_PRINTF_CHECK(1, 2);

// Full path of the current log file (for "Open Event Log" in the tray menu /
// `steamlessctl log` on Linux).
std::string FilePath();

}
