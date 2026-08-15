#pragma once
#include "core/iface/ISettingsStore.h"
#include <mutex>
#include <memory>

// TOML-file-backed settings, replacing the Windows registry. A `section`
// string may contain one or more '/'-separated path components (e.g.
// "GameProfiles/0"), each becoming a nested TOML table — SettingsCodec is
// the single place that decides what those component names actually are,
// so this class only needs to know how to navigate a slash-separated path.
//
// Writes are buffered in memory and only hit disk on Flush() (atomic
// write-temp + fsync + rename), so a burst of CLI mutations doesn't thrash
// the file — see Daemon's debounced-persist policy.
class FileSettingsStore : public ISettingsStore {
public:
    explicit FileSettingsStore(std::string path);
    ~FileSettingsStore() override;

    std::optional<uint32_t>    GetUInt  (std::string_view section, std::string_view key) const override;
    std::optional<std::string> GetString(std::string_view section, std::string_view key) const override;
    void SetUInt  (std::string_view section, std::string_view key, uint32_t value) override;
    void SetString(std::string_view section, std::string_view key, const std::string& value) override;
    std::vector<std::string> ListSubsections(std::string_view section) const override;
    void RemoveSection(std::string_view section) override;

    bool PrefersText() const override { return true; }
    bool SupportsLegacyMigration() const override { return false; }

    bool Flush() override;

private:
    void Load();

    // toml++'s `table` type is kept entirely inside the .cpp (Impl) rather
    // than forward-declared here — toml++ v3 lives in an inline-versioned
    // `toml::v3` namespace, and a bare `namespace toml { class table; }`
    // forward declaration collides with that once both are visible in the
    // same translation unit.
    struct Impl;
    std::string          m_path;
    mutable std::mutex   m_mutex;
    std::unique_ptr<Impl> m_impl;
    bool                  m_dirty = false;
};
