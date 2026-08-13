// Console diagnostic: dumps what GameLibrary::EnumerateInstalled() finds by
// walking the Start Menu, so the path-matching approach can be checked
// against a real machine's installed games without going through the
// remap window's UI.
#include "app/GameLibrary.h"
#include <cstdio>

int main() {
    const auto games = GameLibrary::EnumerateInstalled();
    wprintf(L"%zu game(s) found:\n", games.size());
    for (const auto& g : games)
        wprintf(L"  %-40ls  %ls\n", g.name.c_str(), g.id.c_str());
    return 0;
}
