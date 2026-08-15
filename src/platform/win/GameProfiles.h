#pragma once
#include <map>
#include <string>
#include "core/TrackpadConfig.h"

// Per-game overrides, keyed by the game's id (the same opaque identity
// GameLibrary::InstalledGame::id produces — an exe path for most shortcuts, a
// "steam://rungameid/N" URI for Steam games, an "aumid:..." for packaged
// apps). A game with no entry here uses the global default profile instead —
// most users never create any of these.
namespace GameProfiles {

std::map<std::wstring, ControllerProfile> Load();
void Save(const std::map<std::wstring, ControllerProfile>& profiles);

}
