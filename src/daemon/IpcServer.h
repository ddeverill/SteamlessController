#pragma once
#include "ipc/Json.h"
#include <functional>
#include <string>
#include <vector>

// The daemon side of the control socket: accepts client connections,
// frames newline-delimited JSON off each one, and dispatches complete
// requests to a callback. Also fans out subscribed events to every client
// that asked for them — the direct analog of the "state"/"alert" pushes
// ControllerManager's callbacks feed in (see Daemon).
//
// Single-threaded by design: Poll() is called from the daemon's own main
// loop, so every request handler and every event broadcast happens on the
// one thread that also drives ControllerManager — no locking needed
// anywhere in this class.
// What a command handler answers with — either data on success, or an
// error message + short machine-readable code (e.g. "bad_binding",
// "no_controller") a scripted CLI caller can switch on.
struct CommandResult {
    bool        ok   = true;
    JsonValue   data = JsonValue::Object();
    std::string error;
    std::string code;

    static CommandResult Ok(JsonValue d = JsonValue::Object()) {
        CommandResult r; r.ok = true; r.data = std::move(d); return r;
    }
    static CommandResult Err(std::string message, std::string code = "error") {
        CommandResult r; r.ok = false; r.error = std::move(message); r.code = std::move(code); return r;
    }
};

class IpcServer {
public:
    // req: the parsed request object (already validated as an object with
    // a "cmd" string).
    using CommandHandler = std::function<CommandResult(const JsonValue& req)>;

    ~IpcServer() { Stop(); }

    bool Start(const std::string& socketPath, CommandHandler handler);
    void Stop();

    // Services the listening socket and all client connections once, for up
    // to timeoutMs. Safe to call in a tight loop with timeoutMs=0 from a
    // caller that has its own poll() to wait on elsewhere.
    void Poll(int timeoutMs);

    // Sends {"v":1,"event":...} to every client subscribed to that event
    // name (via the "subscribe" command — see Daemon's handler).
    void Broadcast(const std::string& eventName, JsonValue event);

private:
    struct Client {
        int fd = -1;
        std::string readBuf;
        std::vector<std::string> subscriptions;
        bool helloed = false;
    };

    void AcceptNew();
    void ServiceClient(Client& client);
    void HandleLine(Client& client, const std::string& line);
    void SendTo(Client& client, const JsonValue& msg);

    int                  m_listenFd = -1;
    std::string          m_socketPath;
    CommandHandler       m_handler;
    std::vector<Client>  m_clients;
};
