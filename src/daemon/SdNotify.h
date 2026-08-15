#pragma once
#include <string>

// A direct, ~15-line implementation of the systemd sd_notify() protocol —
// no libsystemd dependency. Silently does nothing when $NOTIFY_SOCKET isn't
// set, so the daemon behaves identically whether started by systemd
// (Type=notify) or run by hand in a terminal.
namespace SdNotify {
void Send(const std::string& state);  // e.g. "READY=1", "STATUS=..."
}
