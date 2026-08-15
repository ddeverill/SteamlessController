#include "Daemon.h"
#include "SdNotify.h"
#include "core/BindingText.h"
#include "core/EventLog.h"
#include "core/KeyNames.h"
#include "core/iface/IDeviceMonitor.h"
#include "core/iface/IForegroundWatcher.h"
#include "core/iface/IGameLibrary.h"
#include "core/iface/IInputInjector.h"
#include "core/iface/IOnScreenKeyboard.h"
#include "core/iface/IDeviceReclaimer.h"
#include "core/iface/IScheduler.h"
#include "core/iface/ISettingsStore.h"
#include "core/iface/ISteamWatcher.h"
#include "core/iface/IVirtualGamepad.h"
#include "ipc/UnixSocket.h"
#include <algorithm>
#include <cstdio>

namespace {

JsonValue BindingsToJson(const BackButtonConfig& back) {
    JsonValue o = JsonValue::Object();
    o["l4"] = BindingText::Format(back.l4);
    o["l5"] = BindingText::Format(back.l5);
    o["r4"] = BindingText::Format(back.r4);
    o["r5"] = BindingText::Format(back.r5);
    return o;
}

JsonValue PadToJson(const TrackpadSettings& pad) {
    JsonValue o = JsonValue::Object();
    o["mode"] = TrackpadModeId(pad.mode);
    o["scroll"] = ScrollDirectionId(pad.scrollDir);
    o["click"] = BindingText::Format(pad.click);
    return o;
}

JsonValue ProfileToJson(const ControllerProfile& p) {
    JsonValue o = JsonValue::Object();
    o["platform"] = p.platform == ControllerPlatform::PlayStation ? "ds4" : "xbox";
    o["back"] = BindingsToJson(p.back);
    o["leftPad"] = PadToJson(p.leftPad);
    o["rightPad"] = PadToJson(p.rightPad);
    if (!p.displayName.empty()) o["name"] = p.displayName;
    return o;
}

BackButtonBinding* SelectPaddle(BackButtonConfig& back, const std::string& name) {
    if (name == "l4") return &back.l4;
    if (name == "l5") return &back.l5;
    if (name == "r4") return &back.r4;
    if (name == "r5") return &back.r5;
    return nullptr;
}

}  // namespace

Daemon::Daemon(LinuxPlatform& platform)
    : m_platform(platform)
    , m_controller(platform, [this](bool connected, bool gameModeActive, bool padUnavailable) {
          OnControllerStateChanged(connected, gameModeActive, padUnavailable);
      })
{
    m_controller.SetAlertCallback([this](const std::string& title, const std::string& text) {
        OnAlert(title, text);
    });

    LoadSettingsFromStore();
    m_wantControl = true;  // "the daemon is running" defaults to "it wants the controller"
    ApplyActiveProfile();

    m_platform.Steam().Start([this](SteamState s) {
        m_platform.Scheduler().Post([this, s] { OnSteamStateChanged(s); });
    });
    m_platform.Devices().Start([this] {
        m_platform.Scheduler().Post([this] { m_controller.OnDeviceChange(); });
    });

    m_ipc.Start(UnixSocket::DefaultSocketPath(), [this](const JsonValue& req) {
        return HandleCommand(req);
    });

    EventLog::Write("DAEMON: started, socket=%s", UnixSocket::DefaultSocketPath().c_str());
}

void Daemon::Run() {
    SdNotify::Send("READY=1\nSTATUS=running");
    while (m_running.load()) {
        // Run any due timers/posted callbacks first — this is the only
        // place ControllerManager is ever touched from, keeping every
        // background thread's work serialized onto this one loop.
        const int nextMs = m_platform.GetScheduler().RunDuePending();
        const int waitMs = nextMs < 0 ? 200 : std::min(nextMs, 200);
        m_ipc.Poll(waitMs);
    }
    SdNotify::Send("STOPPING=1");
    EventLog::Write("DAEMON: stopping");
    if (m_settingsDirty) PersistSettings();
}

// ---------------------------------------------------------------------------
// ControllerManager callbacks
// ---------------------------------------------------------------------------

void Daemon::OnControllerStateChanged(bool connected, bool gameModeActive, bool padUnavailable) {
    JsonValue e = JsonValue::Object();
    e["connected"]      = connected;
    e["gameMode"]       = gameModeActive;
    e["padUnavailable"] = padUnavailable;
    e["platform"]       = EffectiveProfile().platform == ControllerPlatform::PlayStation ? "ds4" : "xbox";
    e["profile"]        = m_activeGameId;
    m_ipc.Broadcast("state", e);
}

void Daemon::OnAlert(const std::string& title, const std::string& text) {
    EventLog::Write("ALERT: %s — %s", title.c_str(), text.c_str());
    if (!m_settings.notifications) return;
    JsonValue e = JsonValue::Object();
    e["severity"] = "warning";
    e["title"] = title;
    e["text"] = text;
    m_ipc.Broadcast("alert", e);
}

void Daemon::OnSteamStateChanged(SteamState state) {
    m_lastSteamState = state;
    m_controller.SetSteamPresent(state != SteamState::NoSteam);

    bool shouldHold;
    switch (m_settings.autoMode) {
    case AutoMode::OffWhileSteam: shouldHold = m_wantControl && state == SteamState::NoSteam; break;
    case AutoMode::OffOnlyInGame: shouldHold = m_wantControl && state != SteamState::InGame;   break;
    default:                      shouldHold = m_wantControl; break;
    }

    if (shouldHold && !m_controller.IsGameModeActive()) {
        m_controller.OnDeviceChange();  // re-open any slot ReleaseDevices() closed
        m_controller.EnableGameMode();
    } else if (!shouldHold && m_controller.IsGameModeActive()) {
        m_controller.ReleaseDevices();
    }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void Daemon::LoadSettingsFromStore() {
    ISettingsStore& store = m_platform.Settings();
    m_settings       = SettingsCodec::LoadAppSettings(store);
    m_defaultProfile = SettingsCodec::LoadDefaultProfile(store);
    m_gameProfiles   = SettingsCodec::LoadGameProfiles(store);
}

void Daemon::PersistSettings() {
    ISettingsStore& store = m_platform.Settings();
    SettingsCodec::SaveAppSettings(store, m_settings);
    SettingsCodec::SaveDefaultProfile(store, m_defaultProfile);
    SettingsCodec::SaveGameProfiles(store, m_gameProfiles);
    store.Flush();
    m_settingsDirty = false;
    EventLog::Write("CONFIG: saved");
}

namespace {
constexpr auto kPersistDebounce = std::chrono::milliseconds(250);
}

void Daemon::ApplyActiveProfile() {
    m_controller.SetProfile(EffectiveProfile());

    m_settingsDirty = true;
    m_platform.Scheduler().Cancel(m_persistTimer);
    m_persistTimer = m_platform.Scheduler().After(kPersistDebounce, [this] {
        if (m_settingsDirty) PersistSettings();
    });
}

const ControllerProfile& Daemon::EffectiveProfile() const {
    if (!m_activeGameId.empty()) {
        for (auto& e : m_gameProfiles)
            if (e.id == m_activeGameId) return e.profile;
    }
    return m_defaultProfile;
}

SettingsCodec::GameProfileEntry* Daemon::FindGameProfile(const std::string& id) {
    for (auto& e : m_gameProfiles) if (e.id == id) return &e;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

JsonValue Daemon::BuildStatus() const {
    JsonValue o = JsonValue::Object();
    o["connected"] = m_controller.IsConnected();
    o["gameMode"]  = m_controller.IsGameModeActive();
    o["platform"]  = EffectiveProfile().platform == ControllerPlatform::PlayStation ? "ds4" : "xbox";
    o["profile"]   = m_activeGameId;
    o["autoMode"]  = m_settings.autoMode == AutoMode::OffWhileSteam ? "off-while-steam"
                    : m_settings.autoMode == AutoMode::OffOnlyInGame ? "off-only-in-game" : "manual";
    o["steamState"] = m_lastSteamState == SteamState::InGame ? "in-game"
                     : m_lastSteamState == SteamState::SteamIdle ? "steam-idle" : "no-steam";
    o["padDriverMissing"] = m_controller.LastPadDriverMissing();
    if (!m_controller.LastBusReport().empty()) o["busReport"] = m_controller.LastBusReport();
    o["wantControl"] = m_wantControl;
    return o;
}

JsonValue Daemon::BuildDevices() const {
    // ControllerManager doesn't expose per-slot enumeration today (it only
    // reports aggregate connected/gameMode state) — a fuller `devices`
    // command that lists each slot's transport/serial/battery is follow-on
    // work; for now this reports what the aggregate state already knows.
    JsonValue arr = JsonValue::Array();
    if (m_controller.IsConnected()) {
        JsonValue d = JsonValue::Object();
        d["gameMode"] = m_controller.IsGameModeActive();
        arr.push_back(d);
    }
    return arr;
}

JsonValue Daemon::BuildDoctor() const {
    JsonValue o = JsonValue::Object();
    o["uinputAvailable"] = m_platform.Gamepads().BusAvailable();
    o["uinputReport"]    = m_platform.Gamepads().BusReport();
    o["foregroundSupport"] =
        m_platform.Foreground().Availability() == IForegroundWatcher::Support::Reliable   ? "reliable"
      : m_platform.Foreground().Availability() == IForegroundWatcher::Support::BestEffort  ? "best-effort"
                                                                                            : "unavailable";
    o["deviceReclaimSupported"] = m_platform.Reclaimer().Available();
    o["oskAvailable"] = m_platform.Osk().Available();
    return o;
}

CommandResult Daemon::HandleCommand(const JsonValue& req) {
    const std::string cmd  = req.find("cmd") ? req.find("cmd")->asString() : "";
    const JsonValue*  args = req.find("args");
    auto argStr = [&](const char* key, const std::string& def = "") -> std::string {
        if (!args) return def;
        auto* v = args->find(key);
        return v ? v->asString(def) : def;
    };

    // profile arg selects which profile a bind/pad/platform mutation
    // targets: "" (or omitted) is the default profile, otherwise a game id.
    const std::string profileArg = argStr("profile");
    ControllerProfile* target = nullptr;
    if (profileArg.empty()) {
        target = &m_defaultProfile;
    } else if (auto* entry = FindGameProfile(profileArg)) {
        target = &entry->profile;
    } else {
        return CommandResult::Err("no such profile: " + profileArg, "no_such_profile");
    }

    if (cmd == "status") return CommandResult::Ok(BuildStatus());
    if (cmd == "devices") return CommandResult::Ok(BuildDevices());
    if (cmd == "doctor")  return CommandResult::Ok(BuildDoctor());

    if (cmd == "actions") {
        JsonValue arr = JsonValue::Array();
        for (auto id : { "A","B","X","Y","LB","RB","LT","RT","Up","Down","Left","Right",
                         "menu","view","L3","R3","leftMouse","rightMouse",
                         "mouse:middle","mouse:x1","mouse:x2","touchKeyboard","none" })
            arr.push_back(JsonValue(id));
        return CommandResult::Ok(arr);
    }

    if (cmd == "games") {
        JsonValue arr = JsonValue::Array();
        for (auto& g : m_platform.Games().EnumerateInstalled()) {
            JsonValue o = JsonValue::Object();
            o["name"] = g.name; o["id"] = g.id;
            arr.push_back(o);
        }
        return CommandResult::Ok(arr);
    }

    if (cmd == "platform.get")
        return CommandResult::Ok(JsonValue(target->platform == ControllerPlatform::PlayStation ? "ds4" : "xbox"));
    if (cmd == "platform.set") {
        const std::string v = argStr("value");
        if (v != "xbox" && v != "ds4") return CommandResult::Err("platform must be xbox or ds4", "bad_value");
        target->platform = v == "ds4" ? ControllerPlatform::PlayStation : ControllerPlatform::Xbox;
        ApplyActiveProfile();
        return CommandResult::Ok();
    }

    if (cmd == "bind.list")
        return CommandResult::Ok(BindingsToJson(target->back));
    if (cmd == "bind.get" || cmd == "bind.set" || cmd == "bind.clear") {
        auto* slot = SelectPaddle(target->back, argStr("paddle"));
        if (!slot) return CommandResult::Err("paddle must be l4/l5/r4/r5", "bad_paddle");
        if (cmd == "bind.get")
            return CommandResult::Ok(JsonValue(BindingText::Format(*slot)));
        if (cmd == "bind.clear") { *slot = BackButtonBinding{}; ApplyActiveProfile(); return CommandResult::Ok(); }
        auto parsed = BindingText::Parse(argStr("binding"));
        if (!parsed) return CommandResult::Err("unrecognised binding: " + argStr("binding"), "bad_binding");
        *slot = *parsed;
        ApplyActiveProfile();
        JsonValue data = JsonValue::Object();
        data["display"] = BindingText::Format(*slot);
        return CommandResult::Ok(data);
    }

    if (cmd == "pad.list") {
        JsonValue o = JsonValue::Object();
        o["left"]  = PadToJson(target->leftPad);
        o["right"] = PadToJson(target->rightPad);
        return CommandResult::Ok(o);
    }
    if (cmd == "pad.get" || cmd == "pad.set") {
        const std::string side = argStr("side");
        TrackpadSettings* pad = side == "left" ? &target->leftPad : side == "right" ? &target->rightPad : nullptr;
        if (!pad) return CommandResult::Err("side must be left or right", "bad_side");
        if (cmd == "pad.get") return CommandResult::Ok(PadToJson(*pad));
        if (auto mode = argStr("mode"); !mode.empty()) pad->mode = TrackpadModeFromId(mode);
        if (auto dir  = argStr("scroll"); !dir.empty()) pad->scrollDir = ScrollDirectionFromId(dir);
        if (auto click = argStr("click"); !click.empty()) {
            auto parsed = BindingText::Parse(click);
            if (!parsed) return CommandResult::Err("unrecognised binding: " + click, "bad_binding");
            pad->click = *parsed;
        }
        ApplyActiveProfile();
        return CommandResult::Ok(PadToJson(*pad));
    }

    if (cmd == "profile.list") {
        JsonValue arr = JsonValue::Array();
        JsonValue def = JsonValue::Object();
        def["id"] = ""; def["name"] = "(default)"; def["active"] = m_activeGameId.empty();
        arr.push_back(def);
        for (auto& e : m_gameProfiles) {
            JsonValue o = JsonValue::Object();
            o["id"] = e.id; o["name"] = e.profile.displayName; o["active"] = (e.id == m_activeGameId);
            arr.push_back(o);
        }
        return CommandResult::Ok(arr);
    }
    if (cmd == "profile.show") return CommandResult::Ok(ProfileToJson(*target));
    if (cmd == "profile.create") {
        const std::string id = argStr("id");
        if (id.empty()) return CommandResult::Err("id is required", "bad_request");
        if (FindGameProfile(id)) return CommandResult::Err("profile already exists: " + id, "exists");
        SettingsCodec::GameProfileEntry entry;
        entry.id = id;
        const std::string from = argStr("from", "default");
        if (from == "default") {
            entry.profile = m_defaultProfile;
        } else if (auto* source = FindGameProfile(from)) {
            entry.profile = source->profile;
        } else {
            return CommandResult::Err("no such profile to copy from: " + from, "no_such_profile");
        }
        entry.profile.displayName = argStr("name");
        m_gameProfiles.push_back(std::move(entry));
        ApplyActiveProfile();
        return CommandResult::Ok();
    }
    if (cmd == "profile.delete") {
        const std::string id = argStr("id");
        auto it = std::remove_if(m_gameProfiles.begin(), m_gameProfiles.end(),
                                  [&](auto& e) { return e.id == id; });
        if (it == m_gameProfiles.end()) return CommandResult::Err("no such profile: " + id, "no_such_profile");
        m_gameProfiles.erase(it, m_gameProfiles.end());
        if (m_activeGameId == id) m_activeGameId.clear();
        ApplyActiveProfile();
        return CommandResult::Ok();
    }
    if (cmd == "profile.activate") {
        const std::string id = argStr("id");
        if (!id.empty() && !FindGameProfile(id)) return CommandResult::Err("no such profile: " + id, "no_such_profile");
        m_activeGameId = id;
        ApplyActiveProfile();
        return CommandResult::Ok();
    }

    if (cmd == "mode.get") {
        return CommandResult::Ok(JsonValue(
            m_settings.autoMode == AutoMode::OffWhileSteam ? "off-while-steam"
          : m_settings.autoMode == AutoMode::OffOnlyInGame ? "off-only-in-game" : "manual"));
    }
    if (cmd == "mode.set") {
        const std::string v = argStr("value");
        if (v == "manual") m_settings.autoMode = AutoMode::Manual;
        else if (v == "off-while-steam") m_settings.autoMode = AutoMode::OffWhileSteam;
        else if (v == "off-only-in-game") m_settings.autoMode = AutoMode::OffOnlyInGame;
        else return CommandResult::Err("mode must be manual/off-while-steam/off-only-in-game", "bad_value");
        OnSteamStateChanged(m_lastSteamState);  // re-evaluate immediately under the new policy
        m_settingsDirty = true;
        return CommandResult::Ok();
    }

    if (cmd == "control.acquire" || cmd == "control.release") {
        m_wantControl = (cmd == "control.acquire");
        OnSteamStateChanged(m_lastSteamState);
        return CommandResult::Ok();
    }

    if (cmd == "daemon.status") {
        JsonValue o = JsonValue::Object();
        o["running"] = true;
        return CommandResult::Ok(o);
    }
    if (cmd == "daemon.stop") {
        RequestStop();
        return CommandResult::Ok();
    }

    return CommandResult::Err("unknown command: " + cmd, "unknown_command");
}
