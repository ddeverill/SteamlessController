#pragma once
#include "core/iface/IGameLibrary.h"

// Wraps the original GameLibrary::EnumerateInstalled() (wide-string, Start
// Menu + packaged-app walk — see WinGameLibrary.h/.cpp, unchanged from
// before the port) to satisfy IGameLibrary for WinPlatform. TrayApp and
// RemapWindow keep calling GameLibrary::EnumerateInstalled() directly for
// their own (richer, WinInstalledGame-typed) picker UI; this adapter exists
// only so WinPlatform has a real interface slot.
class WinGameLibraryAdapter : public IGameLibrary {
public:
    std::vector<InstalledGame> EnumerateInstalled() override;
};
