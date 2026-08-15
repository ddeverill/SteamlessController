#pragma once
#include <string>

// Thin wrappers around the Unix domain socket calls shared by the daemon's
// listener and the CLI's client. Socket path convention:
// ${XDG_RUNTIME_DIR}/steamlesscontroller/control.sock (falls back to
// /tmp/steamlesscontroller-<uid>/control.sock if XDG_RUNTIME_DIR is unset —
// documented as a fallback for minimal/non-systemd sessions, not the
// expected common case).
namespace UnixSocket {

std::string DefaultSocketPath();

// Creates the listening socket at `path` (dir mode 0700, socket mode 0600),
// removing a stale socket file first. Returns -1 on failure.
int Listen(const std::string& path);

// Connects to an existing socket. Returns -1 on failure (including "no
// daemon running").
int Connect(const std::string& path);

}  // namespace UnixSocket
