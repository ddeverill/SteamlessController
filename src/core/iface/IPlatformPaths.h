#pragma once
#include <string>
#include <vector>

// Filesystem locations and path-comparison rules that differ per OS.
class IPlatformPaths {
public:
    virtual ~IPlatformPaths() = default;

    // Where EventLog writes. Windows: %LOCALAPPDATA%\SteamlessController.
    // Linux: ~/.local/state/steamlesscontroller (XDG_STATE_HOME).
    virtual std::string StateDir() const = 0;

    // Where FileSettingsStore reads/writes config.toml. Unused on Windows
    // (settings live in the registry). Linux: ~/.config/steamlesscontroller
    // (XDG_CONFIG_HOME).
    virtual std::string ConfigDir() const = 0;

    // Candidate Steam install roots, in probe order.
    virtual std::vector<std::string> SteamRoots() const = 0;

    // Normalizes a path for prefix-matching against Steam library paths.
    // Windows: case-folded (its filesystems are case-insensitive). Linux:
    // identity (case-sensitive filesystems — folding here was a real bug
    // ported from the Windows code, see SteamAppLocator).
    virtual std::string PathKey(const std::string& path) const = 0;
};
