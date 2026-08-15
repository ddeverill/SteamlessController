#pragma once
#include "AppSettings.h"
#include "TrackpadConfig.h"
#include "iface/ISettingsStore.h"
#include <string>
#include <vector>

// The single place that knows how AppSettings/ControllerProfile map onto an
// ISettingsStore's flat section/key shape — shared by WinRegistryStore and
// FileSettingsStore so a Windows registry read and a Linux TOML read
// produce identical in-memory settings. Two deliberate asymmetries, per
// ISettingsStore::PrefersText(): Windows keeps its packed-DWORD encoding
// for BackButtonBinding (existing installs depend on it; see
// BackButtonBinding::Pack()'s backward-compat contract), Linux writes the
// human-readable BackButtonBinding::Id() string form.
namespace SettingsCodec {

// The default profile and app-level settings both live in the store's root
// section ("") — the same flat layout the Windows registry has always used
// directly under Software\SteamlessController.
AppSettings        LoadAppSettings(const ISettingsStore& store);
void               SaveAppSettings(ISettingsStore& store, const AppSettings& settings);
ControllerProfile  LoadDefaultProfile(const ISettingsStore& store);
void               SaveDefaultProfile(ISettingsStore& store, const ControllerProfile& profile);

// One per-game override, keyed by the numbered "GameProfiles/<n>" section
// the registry has always used (exe paths and steam:// URIs contain
// characters that can't be registry/TOML key names themselves).
struct GameProfileEntry {
    std::string       id;    // ControllerProfile has no id field of its own — see InstalledGame::id
    ControllerProfile profile;
};

std::vector<GameProfileEntry> LoadGameProfiles(const ISettingsStore& store);
// Replaces the entire GameProfiles section with exactly this list.
void SaveGameProfiles(ISettingsStore& store, const std::vector<GameProfileEntry>& profiles);

}  // namespace SettingsCodec
