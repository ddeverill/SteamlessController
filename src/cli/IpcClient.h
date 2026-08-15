#pragma once
#include "ipc/Json.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Connects to the daemon's control socket and does one request/response
// round trip per call — steamlessctl is a one-shot process, not a
// long-lived subscriber (except `status --watch`/`log -f`, which keep the
// connection open and read pushed events instead).
class IpcClient {
public:
    // Empty socketPath uses UnixSocket::DefaultSocketPath().
    bool Connect(const std::string& socketPath = "");
    bool Connected() const { return m_fd >= 0; }

    // Sends {"cmd":cmd,"args":args} and waits for the matching response.
    // Returns nullopt on a transport error (daemon not running, disconnect
    // mid-request) — a well-formed {"ok":false,...} response is NOT an
    // error here, callers check ["ok"] themselves.
    std::optional<JsonValue> Request(const std::string& cmd, JsonValue args = JsonValue::Object());

    // For `status --watch`/`log -f`: sends a "subscribe" request, then reads
    // pushed events one at a time, calling onEvent for each until it returns
    // false or the connection drops.
    void SubscribeAndWatch(const std::vector<std::string>& events,
                            const std::function<bool(const JsonValue&)>& onEvent);

private:
    std::optional<JsonValue> ReadOneLine();

    int m_fd = -1;
    int m_nextId = 1;
    std::string m_readBuf;
};
