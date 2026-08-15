#pragma once
#include "core/iface/IGameLibrary.h"
#include "core/iface/IPlatformPaths.h"

// Combines two sources, the Linux analogs of the Windows Start-Menu walk:
//   - .desktop files under the standard XDG application directories, giving
//     ordinary installed programs a "steamlessctl games" entry; and
//   - Steam's own appmanifest_*.acf files (via the same library roots
//     SteamAppLocator uses), identified as "steam://rungameid/N" so the id
//     matches what SteamAppLocator/the foreground watcher can resolve back.
// MSIX/UWP package resolution has no Linux equivalent and is simply absent.
class LinuxGameLibrary : public IGameLibrary {
public:
    explicit LinuxGameLibrary(const IPlatformPaths& paths) : m_paths(paths) {}
    std::vector<InstalledGame> EnumerateInstalled() override;

private:
    const IPlatformPaths& m_paths;
};
