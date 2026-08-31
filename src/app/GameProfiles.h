#pragma once
#include <map>
#include <string>
#include "TrackpadConfig.h"

// Per-game overrides, keyed by the game's id (the same opaque identity
// GameLibrary::InstalledGame::id produces — an exe path for most shortcuts
// and for an application the user picked by hand, a "steam://rungameid/N"
// URI for Steam games, an "aumid:..." for packaged apps, and a
// "dir:C:\path" install directory for a game found through a launcher's own
// records). A game with no entry here uses the global default profile
// instead — most users never create any of these.
namespace GameProfiles {

std::map<std::wstring, ControllerProfile> Load();
void Save(const std::map<std::wstring, ControllerProfile>& profiles);

}
