#include "SettingsCodec.h"
#include "BindingText.h"

namespace {

const char* PlatformId(ControllerPlatform p) {
    return p == ControllerPlatform::PlayStation ? "ds4" : "xbox";
}
ControllerPlatform PlatformFromId(const std::string& id) {
    return id == "ds4" ? ControllerPlatform::PlayStation : ControllerPlatform::Xbox;
}

void SaveBinding(ISettingsStore& store, const std::string& section, const char* key,
                 const BackButtonBinding& b) {
    if (store.PrefersText())
        // BindingText::Format, not b.Id(): Id() gives Kind::Key its packed-
        // integer wire form ("key:65603"), which round-trips fine but is
        // exactly the user-hostile spelling a hand-editable TOML file is
        // supposed to avoid. Format() spells the same binding as
        // "key:ctrl+KeyC".
        store.SetString(section, key, BindingText::Format(b));
    else
        store.SetUInt(section, key, b.Pack());
}

BackButtonBinding LoadBinding(const ISettingsStore& store, const std::string& section, const char* key,
                              BackButtonBinding fallback) {
    if (store.PrefersText()) {
        // BindingText::Parse accepts both its own readable spelling and the
        // legacy "key:<n>" packed form, so a config written by an older
        // build (or hand-edited back to the packed form) still loads.
        if (auto s = store.GetString(section, key)) {
            if (auto parsed = BindingText::Parse(*s)) return *parsed;
            return fallback;
        }
    } else {
        if (auto v = store.GetUInt(section, key)) return BackButtonBinding::Unpack(*v);
    }
    return fallback;
}

void SaveTrackpad(ISettingsStore& store, const std::string& section, const char* prefix,
                  const TrackpadSettings& pad) {
    store.SetString(section, (std::string(prefix) + "Mode").c_str(), TrackpadModeId(pad.mode));
    store.SetString(section, (std::string(prefix) + "ScrollDir").c_str(), ScrollDirectionId(pad.scrollDir));
    SaveBinding(store, section, (std::string(prefix) + "Click").c_str(), pad.click);
}

TrackpadSettings LoadTrackpad(const ISettingsStore& store, const std::string& section, const char* prefix,
                              const TrackpadSettings& fallback) {
    TrackpadSettings out = fallback;
    if (auto s = store.GetString(section, (std::string(prefix) + "Mode").c_str()))
        out.mode = TrackpadModeFromId(*s);
    if (auto s = store.GetString(section, (std::string(prefix) + "ScrollDir").c_str()))
        out.scrollDir = ScrollDirectionFromId(*s);
    out.click = LoadBinding(store, section, (std::string(prefix) + "Click").c_str(), fallback.click);
    return out;
}

void SaveProfileAt(ISettingsStore& store, const std::string& section, const ControllerProfile& p) {
    store.SetString(section, "Platform", PlatformId(p.platform));
    SaveBinding(store, section, "BackBtnL4", p.back.l4);
    SaveBinding(store, section, "BackBtnL5", p.back.l5);
    SaveBinding(store, section, "BackBtnR4", p.back.r4);
    SaveBinding(store, section, "BackBtnR5", p.back.r5);
    SaveTrackpad(store, section, "LeftPad",  p.leftPad);
    SaveTrackpad(store, section, "RightPad", p.rightPad);
}

ControllerProfile LoadProfileAt(const ISettingsStore& store, const std::string& section,
                                const ControllerProfile& fallback) {
    ControllerProfile p = fallback;
    if (auto s = store.GetString(section, "Platform")) p.platform = PlatformFromId(*s);
    p.back.l4 = LoadBinding(store, section, "BackBtnL4", fallback.back.l4);
    p.back.l5 = LoadBinding(store, section, "BackBtnL5", fallback.back.l5);
    p.back.r4 = LoadBinding(store, section, "BackBtnR4", fallback.back.r4);
    p.back.r5 = LoadBinding(store, section, "BackBtnR5", fallback.back.r5);
    p.leftPad  = LoadTrackpad(store, section, "LeftPad",  fallback.leftPad);
    p.rightPad = LoadTrackpad(store, section, "RightPad", fallback.rightPad);
    return p;
}

const char* AutoModeId(AutoMode m) {
    switch (m) {
    case AutoMode::OffWhileSteam: return "off-while-steam";
    case AutoMode::OffOnlyInGame: return "off-only-in-game";
    default:                      return "manual";
    }
}
AutoMode AutoModeFromId(const std::string& id) {
    if (id == "off-while-steam") return AutoMode::OffWhileSteam;
    if (id == "off-only-in-game") return AutoMode::OffOnlyInGame;
    return AutoMode::Manual;
}

}  // namespace

AppSettings SettingsCodec::LoadAppSettings(const ISettingsStore& store) {
    AppSettings s;
    if (auto v = store.GetString("", "AutoSteamMode")) s.autoMode = AutoModeFromId(*v);
    else if (auto n = store.GetUInt("", "AutoSteamMode")) s.autoMode = static_cast<AutoMode>(*n);
    if (auto v = store.GetUInt("", "ShowNotifications")) s.notifications = (*v != 0);
    return s;
}

void SettingsCodec::SaveAppSettings(ISettingsStore& store, const AppSettings& settings) {
    if (store.PrefersText())
        store.SetString("", "AutoSteamMode", AutoModeId(settings.autoMode));
    else
        store.SetUInt("", "AutoSteamMode", static_cast<uint32_t>(settings.autoMode));
    store.SetUInt("", "ShowNotifications", settings.notifications ? 1 : 0);
}

ControllerProfile SettingsCodec::LoadDefaultProfile(const ISettingsStore& store) {
    return LoadProfileAt(store, "", ControllerProfile{});
}

void SettingsCodec::SaveDefaultProfile(ISettingsStore& store, const ControllerProfile& profile) {
    SaveProfileAt(store, "", profile);
}

std::vector<SettingsCodec::GameProfileEntry> SettingsCodec::LoadGameProfiles(const ISettingsStore& store) {
    std::vector<GameProfileEntry> out;
    for (const auto& n : store.ListSubsections("GameProfiles")) {
        const std::string section = "GameProfiles/" + n;
        GameProfileEntry entry;
        entry.id = store.GetString(section, "Id").value_or("");
        if (entry.id.empty()) continue;  // an unnamed/corrupt entry names nothing to match against
        entry.profile.displayName = store.GetString(section, "Name").value_or("");
        entry.profile = LoadProfileAt(store, section, entry.profile);
        out.push_back(std::move(entry));
    }
    return out;
}

void SettingsCodec::SaveGameProfiles(ISettingsStore& store, const std::vector<GameProfileEntry>& profiles) {
    store.RemoveSection("GameProfiles");
    for (size_t i = 0; i < profiles.size(); ++i) {
        const std::string section = "GameProfiles/" + std::to_string(i);
        store.SetString(section, "Id", profiles[i].id);
        store.SetString(section, "Name", profiles[i].profile.displayName);
        SaveProfileAt(store, section, profiles[i].profile);
    }
}
