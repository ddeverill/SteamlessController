#pragma once
#include <string>
#include <vector>

// Where an entry came from. Carried purely for diagnosis: "my game isn't in
// the list" is the most common report this feature attracts, and the answer
// is almost always that one source came back empty. GameLibraryProbe prints
// a per-source count, which is the thing worth asking a reporter for.
enum class GameSource {
    StartMenu,   // a .lnk under Start Menu or Desktop, resolved to its target
    Steam,       // a Steam .url Internet Shortcut
    Packaged,    // an MSIX/UWP app from PackageManager
    Epic,        // an Epic Games Launcher .item manifest
    Gog,         // a GOG Galaxy registry entry
    Ubisoft,     // a Ubisoft Connect registry entry
    BattleNet,   // a Battle.net uninstall registry entry
    Manual,      // the user pointed at this one themselves
    // Not a source at all, but the absence of one: a saved profile names this
    // game and no source accounts for it, so the picker carries an entry for
    // it anyway. That is what lets a profile for something uninstalled still
    // be looked at and deleted. Never produced by EnumerateInstalled.
    Missing,
};

const wchar_t* GameSourceName(GameSource source);

// One installed application. Called a "game" for the feature it serves, but
// nothing here can actually tell a game from any other program — that
// filtering is left to the user picking from the list.
struct InstalledGame {
    std::wstring name;  // a friendly name, e.g. "Rainbow Six Siege"
    // Opaque stable identity for this game — what per-game profiles are keyed
    // on. Takes one of four shapes depending on where the entry came from:
    //   - a resolved exe path, for an ordinary .lnk shortcut or an
    //     application the user picked by hand;
    //   - "steam://rungameid/N", for a Steam game — Steam shortcuts its
    //     library with .url Internet Shortcuts rather than .lnk files, and
    //     that URI identifies the game precisely (matches the RunningAppID
    //     SteamWatcher already reads) without needing to locate its install
    //     directory;
    //   - "aumid:PackageFamilyName!AppId", for a packaged/MSIX app (Xbox app
    //     and Microsoft Store titles among them) resolved through
    //     PackageManager rather than any shortcut file, since those apps
    //     register no shortcut file for the Start Menu walk to find;
    //   - "dir:C:\path\to\install", for a game found through a launcher's own
    //     records. Those name an install directory rather than an executable,
    //     and the directory is the better matcher anyway: a game's folder
    //     holds its launcher, its anti-cheat service and the game itself, and
    //     which of those reaches the foreground varies by title and by
    //     moment. Matched by longest path prefix, the same rule
    //     SteamAppLocator already applies to Steam games.
    std::wstring id;
    GameSource   source = GameSource::StartMenu;
};

namespace GameLibrary {

// True for an executable that lives under the Windows directory, which is
// this app's one rule for "part of Windows rather than something the user
// installed". Every place a program can reach the picker applies it: the
// shortcut walk, where accessories and Control Panel applets would otherwise
// crowd out real applications; the browse dialog, which refuses them; and the
// running-application list, where the shell itself (explorer.exe), the host
// packaged apps are displayed inside (ApplicationFrameHost.exe) and the touch
// keyboard (TextInputHost.exe) all have windows that qualify as "running" and
// none of which anyone wants a controller profile for.
bool IsSystemProgram(const std::wstring& exePath);

// What to call an executable the user picked themselves, where there is no
// shortcut to take a friendly name from. Prefers the FileDescription in the
// binary's own version resource ("Among Us" for AmongUs.exe), falling back to
// the filename without its extension. Never empty for a real path.
std::wstring NameForExecutable(const std::wstring& exePath);

// Every application this can find, sorted by name, one entry per distinct
// game. Draws on, in order:
//   - both Start Menu Programs folders and both Desktop folders (current
//     user and all users), walked for .lnk shortcuts (resolved to a target
//     executable) and .url shortcuts (resolved to a Steam game's
//     steam://rungameid/N);
//   - PackageManager, for packaged/MSIX apps — Xbox app and Microsoft Store
//     titles among them — which register no shortcut file in those folders
//     for the walk above to find; and
//   - LauncherGames, for the Epic Games Store, GOG Galaxy, Ubisoft Connect
//     and Battle.net, none of which the shortcut walk can describe correctly
//     on its own.
//
// Broad coverage from one code path, at the cost of occasionally surfacing a
// launcher stub instead of the real game and of not being able to tell an
// actual game apart from any other installed program — that filtering is
// left to the user picking from the list. Takes a second or more: call it
// off the UI thread.
std::vector<InstalledGame> EnumerateInstalled();

// The applications with a window open right now, one entry per application,
// each with GameSource::Manual and its executable path as its id.
//
// A separate question from EnumerateInstalled: this is what the remap window
// offers under "add an app that's running now", for the game a user cannot
// find in the list but is looking straight at. Answering it by window rather
// than by process is deliberate — it is the same population the user sees
// when they alt-tab, and a process with no window is not something they could
// have been playing.
//
// Cheap enough to call on the UI thread: one EnumWindows pass and a process
// query per window, with no disk or registry access anywhere in it.
std::vector<InstalledGame> EnumerateRunning();

}
