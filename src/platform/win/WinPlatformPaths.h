#pragma once
#include "core/iface/IPlatformPaths.h"

class WinPlatformPaths : public IPlatformPaths {
public:
    std::string StateDir()  const override;
    std::string ConfigDir() const override;  // unused on Windows (settings live in the registry)
    std::vector<std::string> SteamRoots() const override;
    // Windows filesystems fold case; matching Steam library paths without
    // doing the same here was fine when everything was already lowercased
    // internally, but nothing here does that any more — see SteamAppLocator.
    std::string PathKey(const std::string& path) const override;
};
