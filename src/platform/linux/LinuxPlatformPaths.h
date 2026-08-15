#pragma once
#include "core/iface/IPlatformPaths.h"

class LinuxPlatformPaths : public IPlatformPaths {
public:
    std::string StateDir()  const override;
    std::string ConfigDir() const override;
    std::vector<std::string> SteamRoots() const override;
    // Identity: Linux filesystems are case-sensitive, so folding case before
    // a prefix-match (as the Windows path does, correctly, for its
    // case-insensitive filesystems) would produce false matches — see
    // SteamAppLocator's case-fold comment.
    std::string PathKey(const std::string& path) const override { return path; }
};
