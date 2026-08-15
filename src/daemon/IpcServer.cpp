#include "IpcServer.h"
#include "ipc/UnixSocket.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

bool IpcServer::Start(const std::string& socketPath, CommandHandler handler) {
    Stop();
    m_socketPath = socketPath;
    m_handler    = std::move(handler);
    m_listenFd   = UnixSocket::Listen(socketPath);
    return m_listenFd >= 0;
}

void IpcServer::Stop() {
    for (auto& c : m_clients) if (c.fd >= 0) close(c.fd);
    m_clients.clear();
    if (m_listenFd >= 0) { close(m_listenFd); m_listenFd = -1; }
    if (!m_socketPath.empty()) unlink(m_socketPath.c_str());
}

void IpcServer::Poll(int timeoutMs) {
    if (m_listenFd < 0) return;

    std::vector<struct pollfd> fds;
    fds.push_back({ m_listenFd, POLLIN, 0 });
    for (auto& c : m_clients) fds.push_back({ c.fd, POLLIN, 0 });

    if (poll(fds.data(), fds.size(), timeoutMs) <= 0) return;

    if (fds[0].revents & POLLIN) AcceptNew();

    for (size_t i = 1; i < fds.size(); ++i) {
        if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        ServiceClient(m_clients[i - 1]);
    }

    // Drop anything ServiceClient marked dead (fd set to -1) — done as a
    // separate pass so the index arithmetic above isn't disturbed mid-loop.
    m_clients.erase(std::remove_if(m_clients.begin(), m_clients.end(),
                                    [](const Client& c) { return c.fd < 0; }),
                    m_clients.end());
}

void IpcServer::AcceptNew() {
    const int fd = accept4(m_listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return;
    Client c;
    c.fd = fd;
    m_clients.push_back(std::move(c));
}

void IpcServer::ServiceClient(Client& client) {
    char buf[4096];
    for (;;) {
        const ssize_t n = read(client.fd, buf, sizeof(buf));
        if (n > 0) {
            client.readBuf.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) { close(client.fd); client.fd = -1; return; }  // peer closed
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        close(client.fd); client.fd = -1; return;
    }

    for (;;) {
        const size_t nl = client.readBuf.find('\n');
        if (nl == std::string::npos) break;
        const std::string line = client.readBuf.substr(0, nl);
        client.readBuf.erase(0, nl + 1);
        if (!line.empty()) HandleLine(client, line);
        if (client.fd < 0) return;  // HandleLine can close on a fatal protocol error
    }
}

void IpcServer::HandleLine(Client& client, const std::string& line) {
    auto parsed = JsonValue::parse(line);
    if (!parsed || !parsed->isObject()) {
        JsonValue resp = JsonValue::Object();
        resp["v"] = 1; resp["ok"] = false; resp["error"] = "malformed request"; resp["code"] = "bad_request";
        SendTo(client, resp);
        return;
    }
    const JsonValue& req = *parsed;
    const std::string cmd = req.find("cmd") ? req.find("cmd")->asString() : "";

    if (cmd == "hello") {
        client.helloed = true;
        JsonValue resp = JsonValue::Object();
        resp["v"] = 1;
        if (auto id = req.find("id")) resp["id"] = id->asInt();
        resp["ok"] = true;
        JsonValue data = JsonValue::Object();
        data["v"] = 1;
        data["server"] = "steamless-controllerd";
        resp["data"] = data;
        SendTo(client, resp);
        return;
    }

    if (cmd == "subscribe") {
        if (auto events = req.find("args") ? req.find("args")->find("events") : nullptr) {
            for (auto& e : events->items())
                client.subscriptions.push_back(e.asString());
        }
        JsonValue resp = JsonValue::Object();
        resp["v"] = 1;
        if (auto id = req.find("id")) resp["id"] = id->asInt();
        resp["ok"] = true;
        SendTo(client, resp);
        return;
    }

    if (!m_handler) return;
    const CommandResult result = m_handler(req);

    JsonValue resp = JsonValue::Object();
    resp["v"] = 1;
    if (auto id = req.find("id")) resp["id"] = id->asInt();
    resp["ok"] = result.ok;
    if (result.ok) {
        resp["data"] = result.data;
    } else {
        resp["error"] = result.error;
        resp["code"]  = result.code.empty() ? "error" : result.code;
    }
    SendTo(client, resp);
}

void IpcServer::SendTo(Client& client, const JsonValue& msg) {
    if (client.fd < 0) return;
    std::string line = msg.dump();
    line += '\n';
    size_t sent = 0;
    while (sent < line.size()) {
        const ssize_t n = write(client.fd, line.data() + sent, line.size() - sent);
        if (n <= 0) { close(client.fd); client.fd = -1; return; }
        sent += static_cast<size_t>(n);
    }
}

void IpcServer::Broadcast(const std::string& eventName, JsonValue event) {
    // Flat shape per the wire protocol (an event's own fields sit alongside
    // "v"/"event", not nested under "data" the way a request/response's
    // payload is) — e.g. {"v":1,"event":"state","connected":true,...}.
    JsonValue msg = JsonValue::Object();
    msg["v"] = 1;
    msg["event"] = eventName;
    if (event.isObject())
        for (auto& [k, v] : event.entries()) msg[k] = v;

    const std::string line = msg.dump() + "\n";
    for (auto& client : m_clients) {
        if (!client.helloed) continue;
        if (std::find(client.subscriptions.begin(), client.subscriptions.end(), eventName)
                == client.subscriptions.end())
            continue;
        size_t sent = 0;
        while (client.fd >= 0 && sent < line.size()) {
            const ssize_t n = write(client.fd, line.data() + sent, line.size() - sent);
            if (n <= 0) { close(client.fd); client.fd = -1; break; }
            sent += static_cast<size_t>(n);
        }
    }
}
