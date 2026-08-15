#pragma once
#include "core/iface/ISettingsStore.h"

// Registry-backed ISettingsStore under HKCU\Software\SteamlessController.
// A `section` string's '/'-separated components become nested registry
// subkeys (e.g. "GameProfiles/0" -> ...\GameProfiles\0), and "" addresses
// the base key directly — the same flat shape TrayApp's own LoadSettings/
// SaveSettings and GameProfiles::Load/Save already use directly against the
// registry (this class is not currently wired into either of those; see the
// class comment in WinPlatform.h for why).
class WinRegistryStore : public ISettingsStore {
public:
    std::optional<uint32_t>    GetUInt  (std::string_view section, std::string_view key) const override;
    std::optional<std::string> GetString(std::string_view section, std::string_view key) const override;
    void SetUInt  (std::string_view section, std::string_view key, uint32_t value) override;
    void SetString(std::string_view section, std::string_view key, const std::string& value) override;
    std::vector<std::string> ListSubsections(std::string_view section) const override;
    void RemoveSection(std::string_view section) override;

    bool PrefersText() const override { return false; }
    bool SupportsLegacyMigration() const override { return true; }
    bool Flush() override { return true; }  // registry writes are immediate
};
