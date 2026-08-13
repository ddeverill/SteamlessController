// Console diagnostic: dumps what SteamAppLocator reads out of Steam's own
// manifests, and resolves any paths given on the command line back to an app
// id. The parsing has no visible effect until a profile fails to match, which
// is far too late to find out it read the wrong key.
#include "app/SteamAppLocator.h"
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    SteamAppLocator locator;
    locator.Refresh();

    wprintf(L"%zu installed Steam app(s)\n", locator.Count());

    for (int i = 1; i < argc; ++i) {
        const std::wstring appId = locator.AppIdForPath(argv[i]);
        wprintf(L"  %ls\n    -> %ls\n", argv[i],
                appId.empty() ? L"(no Steam app owns this path)" : appId.c_str());
    }
    return 0;
}
