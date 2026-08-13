#pragma once
#include <map>
#include <string>
#include <vector>

// Maps Steam app ids to where their games are installed, so a running process
// can be traced back to the Steam game it belongs to.
//
// This exists because the two ends do not speak the same language. A Steam
// profile is keyed by app id — that is all a "steam://rungameid/N" shortcut
// carries — while the only thing a foreground window yields is an executable
// path. Steam publishes the missing half on disk: libraryfolders.vdf lists
// every library root, and each steamapps/appmanifest_<id>.acf names the
// folder that app installed into.
//
// Matching on the directory rather than a specific executable is deliberate.
// A game's folder holds its launcher, its anti-cheat service and the game
// itself, and which of those ends up in the foreground varies by title and by
// moment; all of them are equally "that game".
class SteamAppLocator {
public:
    // Re-reads Steam's manifests. Cheap — a handful of small text files — but
    // it is disk I/O, so callers drive it at meaningful moments rather than
    // on every lookup. Safe to call when Steam is not installed at all.
    void Refresh();

    // The app id whose install directory contains this executable, or empty.
    // Longest match wins, so a library that happens to live inside another
    // game's folder still resolves to the more specific one.
    std::wstring AppIdForPath(const std::wstring& exePath) const;

    bool   Empty() const { return m_installDirs.empty(); }
    size_t Count() const { return m_installDirs.size(); }

private:
    // appid -> absolute install directory, lowercased with a trailing
    // separator so a prefix test cannot match a sibling folder whose name
    // merely starts the same way ("Portal" against "Portal 2").
    std::map<std::wstring, std::wstring> m_installDirs;
};
