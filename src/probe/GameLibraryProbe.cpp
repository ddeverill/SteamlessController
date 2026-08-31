// Console diagnostic: dumps what GameLibrary::EnumerateInstalled() finds, so
// the picker's coverage can be checked against a real machine without going
// through the remap window's UI.
//
// The per-source summary at the end is the point of the tool. "My game isn't
// in the list" is the report this feature attracts, and the answer is nearly
// always that one source came back empty — which this shows at a glance and
// a screenshot of the picker never can.
//
// Pass "running" to instead dump what the remap window's "add an app that's
// running now" list would offer, which is the other half of the picker and
// the only way to check it without clicking through the UI:
//     GameLibraryProbe.exe running
//
// Pass a directory to instead run only the Epic manifest reader against it,
// which is how that parser gets exercised on a machine with no Epic install:
//     GameLibraryProbe.exe C:\path\to\fixture\Manifests
#include "app/GameLibrary.h"
#include "app/LauncherGames.h"
#include <Windows.h>
#include <clocale>
#include <cstdio>
#include <map>

namespace {

void Dump(const std::vector<InstalledGame>& games) {
    wprintf(L"%zu entr%ls found:\n", games.size(), games.size() == 1 ? L"y" : L"ies");
    for (const auto& g : games)
        wprintf(L"  %-12ls  %-42ls  %ls\n", GameSourceName(g.source),
                g.name.c_str(), g.id.c_str());

    std::map<std::wstring, size_t> counts;
    for (const auto& g : games) ++counts[GameSourceName(g.source)];
    wprintf(L"\nby source:\n");
    for (const auto& [source, count] : counts)
        wprintf(L"  %-12ls %zu\n", source.c_str(), count);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Plenty of game titles are not ASCII. Without this, wprintf gives up on
    // the first character it cannot encode and abandons the rest of the line,
    // which in a tool whose whole job is showing what was found would quietly
    // hide the entries most worth looking at. Same reason wWinMain sets it.
    setlocale(LC_CTYPE, ".UTF8");
    SetConsoleOutputCP(CP_UTF8);

    if (argc > 1 && _wcsicmp(argv[1], L"running") == 0) {
        wprintf(L"Applications with a window open right now\n\n");
        Dump(GameLibrary::EnumerateRunning());
        return 0;
    }

    if (argc > 1) {
        wprintf(L"Epic manifests from [%ls]\n\n", argv[1]);
        Dump(LauncherGames::EnumerateEpic(argv[1]));
        return 0;
    }

    Dump(GameLibrary::EnumerateInstalled());
    return 0;
}
