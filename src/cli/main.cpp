#include "IpcClient.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool g_json = false;

void PrintUsage() {
    printf(
        "steamlessctl - control the SteamlessController daemon\n\n"
        "Usage: steamlessctl [--json] <command> [args...]\n\n"
        "Commands:\n"
        "  status [-w|--watch]\n"
        "  devices\n"
        "  doctor\n"
        "  actions\n"
        "  games\n"
        "  platform get|set <xbox|ds4> [--profile ID]\n"
        "  bind list|get|set|clear <l4|l5|r4|r5> [<binding>] [--profile ID]\n"
        "  pad list|get|set <left|right> [--mode none|pointer|scroll|ds4]\n"
        "                                [--scroll natural|reversed] [--click <binding>] [--profile ID]\n"
        "  profile list|show|create|delete|activate [ID] [--name NAME] [--from ID]\n"
        "  mode get|set manual|off-while-steam|off-only-in-game\n"
        "  control acquire|release\n"
        "  daemon status|stop\n"
        "  log [-n N]\n\n"
        "Binding syntax: A B X Y LB RB LT RT Up Down Left Right menu view L3 R3\n"
        "  leftMouse rightMouse mouse:middle mouse:x1 mouse:x2 touchKeyboard none\n"
        "  key:[ctrl+][alt+][shift+][win+]<KeyName>   e.g. key:ctrl+alt+KeyC\n");
}

std::string ExtractFlag(std::vector<std::string>& args, const std::string& flag) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == flag && i + 1 < args.size()) {
            std::string v = args[i + 1];
            args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
            return v;
        }
    }
    return "";
}

bool ExtractSwitch(std::vector<std::string>& args, std::initializer_list<const char*> names) {
    for (size_t i = 0; i < args.size(); ++i) {
        for (auto* n : names) {
            if (args[i] == n) { args.erase(args.begin() + static_cast<long>(i)); return true; }
        }
    }
    return false;
}

int Fail(const std::string& msg, int code = 1) {
    if (g_json) {
        JsonValue e = JsonValue::Object();
        e["ok"] = false; e["error"] = msg;
        printf("%s\n", e.dump().c_str());
    } else {
        fprintf(stderr, "error: %s\n", msg.c_str());
    }
    return code;
}

void PrintResult(const JsonValue& resp) {
    if (g_json) { printf("%s\n", resp.dump().c_str()); return; }
    const bool ok = resp.find("ok") && resp.find("ok")->asBool();
    if (!ok) {
        const std::string err = resp.find("error") ? resp.find("error")->asString() : "unknown error";
        fprintf(stderr, "error: %s\n", err.c_str());
        return;
    }
    const JsonValue* data = resp.find("data");
    printf("%s\n", data ? data->dump().c_str() : "ok");
}

int RequireConnection(IpcClient& client) {
    if (!client.Connect())
        return Fail("cannot connect to steamless-controllerd — is it running? "
                    "(systemctl --user status steamlesscontroller)", 3);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    g_json = ExtractSwitch(args, { "--json" });
    const std::string socketOverride = ExtractFlag(args, "--socket");

    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        PrintUsage();
        return args.empty() ? 2 : 0;
    }

    const std::string cmd = args[0];
    args.erase(args.begin());

    // `log` reads the daemon's own log file directly — no socket needed.
    if (cmd == "log") {
        std::string n = ExtractFlag(args, "-n");
        const int lines = n.empty() ? 50 : atoi(n.c_str());
        const char* home = getenv("HOME");
        const char* xdgState = getenv("XDG_STATE_HOME");
        const std::string path = (xdgState && *xdgState ? std::string(xdgState) : std::string(home ? home : "") + "/.local/state")
                                + "/steamlesscontroller/events.log";
        std::ifstream f(path);
        if (!f) return Fail("cannot open log file: " + path);
        std::vector<std::string> all;
        std::string line;
        while (std::getline(f, line)) all.push_back(line);
        const size_t start = all.size() > static_cast<size_t>(lines) ? all.size() - static_cast<size_t>(lines) : 0;
        for (size_t i = start; i < all.size(); ++i) printf("%s\n", all[i].c_str());
        return 0;
    }

    IpcClient client;

    const std::string profile = ExtractFlag(args, "--profile");
    JsonValue baseArgs = JsonValue::Object();
    if (!profile.empty()) baseArgs["profile"] = profile;

    if (cmd == "status") {
        const bool watch = ExtractSwitch(args, { "-w", "--watch" });
        if (int rc = RequireConnection(client)) return rc;
        if (!watch) {
            auto resp = client.Request("status");
            if (!resp) return Fail("daemon did not respond");
            PrintResult(*resp);
            return 0;
        }
        client.SubscribeAndWatch({ "state", "alert" }, [](const JsonValue& msg) {
            printf("%s\n", msg.dump().c_str());
            return true;
        });
        return 0;
    }

    if (cmd == "devices" || cmd == "doctor" || cmd == "actions" || cmd == "games") {
        if (int rc = RequireConnection(client)) return rc;
        auto resp = client.Request(cmd);
        if (!resp) return Fail("daemon did not respond");
        PrintResult(*resp);
        return 0;
    }

    if (cmd == "platform") {
        if (args.empty()) return Fail("usage: platform get|set <xbox|ds4>", 2);
        const std::string sub = args[0];
        if (int rc = RequireConnection(client)) return rc;
        if (sub == "get") {
            auto resp = client.Request("platform.get", baseArgs);
            if (!resp) return Fail("daemon did not respond");
            PrintResult(*resp);
        } else if (sub == "set" && args.size() >= 2) {
            baseArgs["value"] = args[1];
            auto resp = client.Request("platform.set", baseArgs);
            if (!resp) return Fail("daemon did not respond");
            PrintResult(*resp);
        } else {
            return Fail("usage: platform get|set <xbox|ds4>", 2);
        }
        return 0;
    }

    if (cmd == "bind") {
        if (args.empty()) return Fail("usage: bind list|get|set|clear <l4|l5|r4|r5> [<binding>]", 2);
        const std::string sub = args[0];
        if (int rc = RequireConnection(client)) return rc;
        if (sub == "list") {
            auto resp = client.Request("bind.list", baseArgs);
            if (!resp) return Fail("daemon did not respond");
            PrintResult(*resp);
            return 0;
        }
        if (args.size() < 2) return Fail("usage: bind " + sub + " <l4|l5|r4|r5> [<binding>]", 2);
        baseArgs["paddle"] = args[1];
        if (sub == "set") {
            if (args.size() < 3) return Fail("usage: bind set <l4|l5|r4|r5> <binding>", 2);
            baseArgs["binding"] = args[2];
        }
        auto resp = client.Request("bind." + sub, baseArgs);
        if (!resp) return Fail("daemon did not respond");
        PrintResult(*resp);
        return 0;
    }

    if (cmd == "pad") {
        if (args.empty()) return Fail("usage: pad list|get|set <left|right> [--mode ...] [--scroll ...] [--click ...]", 2);
        const std::string sub = args[0];
        const std::string mode = ExtractFlag(args, "--mode");
        const std::string scroll = ExtractFlag(args, "--scroll");
        const std::string click = ExtractFlag(args, "--click");
        if (int rc = RequireConnection(client)) return rc;
        if (sub == "list") {
            auto resp = client.Request("pad.list", baseArgs);
            if (!resp) return Fail("daemon did not respond");
            PrintResult(*resp);
            return 0;
        }
        if (args.size() < 2) return Fail("usage: pad " + sub + " <left|right>", 2);
        baseArgs["side"] = args[1];
        if (!mode.empty())   baseArgs["mode"] = mode;
        if (!scroll.empty()) baseArgs["scroll"] = scroll;
        if (!click.empty())  baseArgs["click"] = click;
        auto resp = client.Request("pad." + sub, baseArgs);
        if (!resp) return Fail("daemon did not respond");
        PrintResult(*resp);
        return 0;
    }

    if (cmd == "profile") {
        if (args.empty()) return Fail("usage: profile list|show|create|delete|activate [ID]", 2);
        const std::string sub = args[0];
        const std::string name = ExtractFlag(args, "--name");
        const std::string from = ExtractFlag(args, "--from");
        const std::string id = args.size() >= 2 ? args[1] : "";
        if (int rc = RequireConnection(client)) return rc;
        JsonValue a = JsonValue::Object();
        if (!id.empty()) a["id"] = id;
        if (!name.empty()) a["name"] = name;
        if (!from.empty()) a["from"] = from;
        if (sub == "show") a = baseArgs;  // "show" targets --profile / default, not a positional id
        auto resp = client.Request("profile." + sub, a);
        if (!resp) return Fail("daemon did not respond");
        PrintResult(*resp);
        return 0;
    }

    if (cmd == "mode") {
        if (args.empty()) return Fail("usage: mode get|set <manual|off-while-steam|off-only-in-game>", 2);
        if (int rc = RequireConnection(client)) return rc;
        JsonValue a = JsonValue::Object();
        if (args[0] == "set" && args.size() >= 2) a["value"] = args[1];
        auto resp = client.Request("mode." + args[0], a);
        if (!resp) return Fail("daemon did not respond");
        PrintResult(*resp);
        return 0;
    }

    if (cmd == "control") {
        if (args.empty() || (args[0] != "acquire" && args[0] != "release"))
            return Fail("usage: control acquire|release", 2);
        if (int rc = RequireConnection(client)) return rc;
        auto resp = client.Request("control." + args[0]);
        if (!resp) return Fail("daemon did not respond");
        PrintResult(*resp);
        return 0;
    }

    if (cmd == "daemon") {
        if (args.empty()) return Fail("usage: daemon status|stop", 2);
        if (args[0] == "status") {
            if (!client.Connect()) {
                if (g_json) printf("{\"running\":false}\n"); else printf("not running\n");
                return 3;
            }
            auto resp = client.Request("daemon.status");
            if (!resp) return Fail("daemon did not respond");
            PrintResult(*resp);
            return 0;
        }
        if (args[0] == "stop") {
            if (int rc = RequireConnection(client)) return rc;
            auto resp = client.Request("daemon.stop");
            (void)resp;
            printf("stop requested\n");
            return 0;
        }
        return Fail("usage: daemon status|stop", 2);
    }

    PrintUsage();
    return Fail("unknown command: " + cmd, 2);
}
