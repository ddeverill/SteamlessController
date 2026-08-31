#pragma once
#include <string>
#include <vector>
#include "GameLibrary.h"

// Games belonging to the PC launchers that the Start Menu walk in GameLibrary
// either misses or describes wrongly.
//
// It misses Epic's, because Epic shortcuts a game with a .url whose URL is a
// "com.epicgames.launcher://apps/..." URI, and the walk only keeps .url files
// pointing at Steam. It describes Battle.net's and Ubisoft's wrongly, because
// those shortcuts are .lnk files targeting the *launcher* executable with a
// per-game argument — IShellLink::GetPath returns only the executable, so
// every Blizzard game on a machine resolves to the same
// "Battle.net Launcher.exe" and collapses into a single wrongly-named entry.
//
// Reading each launcher's own records instead fixes both, and yields the
// game's install directory, which is a better identity than any single
// executable — see the "dir:" note on InstalledGame::id.
//
// Every function here is best-effort: a launcher that is not installed
// returns an empty list rather than failing, exactly as GameLibrary's
// packaged-app pass already does.
namespace LauncherGames {

// All four launchers below, concatenated. Not sorted and not deduplicated —
// GameLibrary does both, over the merged list.
std::vector<InstalledGame> EnumerateAll();

// %ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item, each a JSON
// object describing one installed item. manifestsDir overrides where to look,
// which is the only way to exercise this on a machine without Epic installed.
std::vector<InstalledGame> EnumerateEpic(const std::wstring& manifestsDir = {});

// HKLM\SOFTWARE\GOG.com\Games — one subkey per game, carrying its own name.
std::vector<InstalledGame> EnumerateGog();

// HKLM\SOFTWARE\Ubisoft\Launcher\Installs — one subkey per game, carrying an
// install directory but no name.
std::vector<InstalledGame> EnumerateUbisoft();

// The uninstall registry, filtered to entries Battle.net wrote. Blizzard
// games have no launcher-owned manifest an unprivileged reader can parse
// without a protobuf decoder, and these entries carry everything needed.
std::vector<InstalledGame> EnumerateBattleNet();

// True for a directory specific enough to be worth matching a foreground
// process against. A launcher that reports something like "C:\" or
// "C:\Program Files" would otherwise produce a profile that claims every
// application on the machine. Shared with GameLibrary, which applies the
// same test before trusting a directory from any source.
bool IsUsableInstallDir(const std::wstring& dir);

}  // namespace LauncherGames
