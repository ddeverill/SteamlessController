#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A flat, sectioned key/value store — deliberately shaped like the Windows
// registry, since WinRegistryStore is meant to be a near-literal
// transcription of the registry layout every existing install already has.
// FileSettingsStore (Linux) backs the same shape with a TOML file.
class ISettingsStore {
public:
    virtual ~ISettingsStore() = default;

    virtual std::optional<uint32_t>    GetUInt  (std::string_view section, std::string_view key) const = 0;
    virtual std::optional<std::string> GetString(std::string_view section, std::string_view key) const = 0;
    virtual void SetUInt  (std::string_view section, std::string_view key, uint32_t value) = 0;
    virtual void SetString(std::string_view section, std::string_view key, const std::string& value) = 0;

    // Numbered child sections of `section` (e.g. GameProfiles' "0", "1", ...).
    virtual std::vector<std::string> ListSubsections(std::string_view section) const = 0;
    virtual void RemoveSection(std::string_view section) = 0;

    // True when this store's natural encoding is human-readable text (Linux
    // TOML writes BackButtonBinding::Id() strings) rather than the packed
    // DWORD Windows keeps for backward compatibility with existing installs.
    // SettingsCodec reads this once to decide which encoding to write.
    virtual bool PrefersText() const = 0;

    // Whether this store still needs the BackButtons/PadsMigrated one-time
    // registry migrations SettingsCodec::Load runs for old Windows installs.
    // Always false on a fresh Linux config file.
    virtual bool SupportsLegacyMigration() const = 0;

    // Persist any buffered writes. Registry: no-op (writes are immediate).
    // File-backed: atomic write-temp + fsync + rename.
    virtual bool Flush() = 0;
};
