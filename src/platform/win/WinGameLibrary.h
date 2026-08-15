#pragma once
#include <string>
#include <vector>

// One installed application resolved from a Start Menu shortcut. Called a
// "game" for the feature it serves, but nothing here can actually tell a
// game from any other program — that filtering is left to the user picking
// from the list.
struct WinInstalledGame {
    std::wstring name;  // the shortcut's own name, e.g. "Rainbow Six Siege"
    // Opaque stable identity for this game — what per-game profiles are keyed
    // on. Takes one of three shapes depending on where the entry came from:
    //   - a resolved exe path, for an ordinary Start Menu .lnk;
    //   - "steam://rungameid/N", for a Steam game — Steam shortcuts its
    //     library with .url Internet Shortcuts rather than .lnk files, and
    //     that URI identifies the game precisely (matches the RunningAppID
    //     SteamWatcher already reads) without needing to locate its install
    //     directory;
    //   - "aumid:PackageFamilyName!AppId", for a packaged/MSIX app (Xbox app
    //     and Microsoft Store titles among them) resolved through
    //     shell:AppsFolder rather than any shortcut file, since those apps
    //     register no shortcut file for the Start Menu walk to find.
    std::wstring id;
};

namespace GameLibrary {

// Combines two sources into one list, sorted by name, one entry per distinct
// game:
//   - both Start Menu Programs folders (current user and all users), walked
//     for .lnk shortcuts (resolved to a target executable) and .url
//     shortcuts (resolved to a Steam game's steam://rungameid/N); and
//   - shell:AppsFolder, for packaged/MSIX apps — Xbox app and Microsoft
//     Store titles among them — which register no shortcut file in the
//     Start Menu folders for the walk above to find.
//
// Broad coverage from one code path, at the cost of occasionally surfacing a
// launcher stub instead of the real game (a shortcut that punches out to
// another launcher, for example) and of not being able to tell an actual
// game apart from any other installed program — that filtering is left to
// the user picking from the list.
std::vector<WinInstalledGame> EnumerateInstalled();

}
