// Console diagnostic: dumps what SteamAppLocator reads out of Steam's own
// manifests, and resolves any paths given on the command line back to an app
// id. The parsing has no visible effect until a profile fails to match, which
// is far too late to find out it read the wrong key.
#include "core/SteamAppLocator.h"
#include "core/Text.h"
#include "platform/win/WinPlatformPaths.h"
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    WinPlatformPaths paths;
    SteamAppLocator locator(/*caseInsensitivePaths=*/true);
    locator.SetRoots(paths.SteamRoots());
    locator.Refresh();

    wprintf(L"%zu installed Steam app(s)\n", locator.Count());

    for (int i = 1; i < argc; ++i) {
        const std::string appId = locator.AppIdForPath(WideToUtf8(argv[i]));
        wprintf(L"  %ls\n    -> %ls\n", argv[i],
                appId.empty() ? L"(no Steam app owns this path)" : Utf8ToWide(appId).c_str());
    }
    return 0;
}
