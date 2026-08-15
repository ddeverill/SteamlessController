#include "IpcClient.h"
#include "ipc/UnixSocket.h"
#include <poll.h>
#include <unistd.h>

bool IpcClient::Connect(const std::string& socketPath) {
    m_fd = UnixSocket::Connect(socketPath.empty() ? UnixSocket::DefaultSocketPath() : socketPath);
    return m_fd >= 0;
}

std::optional<JsonValue> IpcClient::ReadOneLine() {
    for (;;) {
        const size_t nl = m_readBuf.find('\n');
        if (nl != std::string::npos) {
            const std::string line = m_readBuf.substr(0, nl);
            m_readBuf.erase(0, nl + 1);
            return JsonValue::parse(line);
        }
        char buf[4096];
        struct pollfd pfd{ m_fd, POLLIN, 0 };
        if (poll(&pfd, 1, 5000) <= 0) return std::nullopt;  // daemon hung or died
        const ssize_t n = read(m_fd, buf, sizeof(buf));
        if (n <= 0) return std::nullopt;
        m_readBuf.append(buf, static_cast<size_t>(n));
    }
}

std::optional<JsonValue> IpcClient::Request(const std::string& cmd, JsonValue args) {
    if (m_fd < 0) return std::nullopt;

    JsonValue req = JsonValue::Object();
    req["v"] = 1;
    const int id = m_nextId++;
    req["id"] = id;
    req["cmd"] = cmd;
    req["args"] = std::move(args);

    const std::string line = req.dump() + "\n";
    size_t sent = 0;
    while (sent < line.size()) {
        const ssize_t n = write(m_fd, line.data() + sent, line.size() - sent);
        if (n <= 0) return std::nullopt;
        sent += static_cast<size_t>(n);
    }

    // The daemon answers requests in order on one connection, so the first
    // line back is always this request's response — no id-matching loop needed.
    return ReadOneLine();
}

void IpcClient::SubscribeAndWatch(const std::vector<std::string>& events,
                                   const std::function<bool(const JsonValue&)>& onEvent) {
    JsonValue args = JsonValue::Object();
    JsonValue arr = JsonValue::Array();
    for (auto& e : events) arr.push_back(JsonValue(e));
    args["events"] = arr;
    if (!Request("subscribe", args)) return;

    for (;;) {
        auto msg = ReadOneLine();
        if (!msg) return;  // disconnected
        if (!msg->find("event")) continue;  // a stray response, not a push
        if (!onEvent(*msg)) return;
    }
}
