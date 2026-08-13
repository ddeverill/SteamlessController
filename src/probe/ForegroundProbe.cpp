// Console diagnostic: prints the foreground application as the user switches
// between windows, which is the signal per-game profiles are resolved from.
// Exists because the interesting failures here are silent — a packaged app
// resolving to ApplicationFrameHost.exe instead of itself looks like nothing
// happening at all.
#include "app/ForegroundWatcher.h"
#include <Windows.h>
#include <cstdio>

static void Print(const wchar_t* tag, const ForegroundIdentity& id) {
    wprintf(L"%-8ls exe=[%ls]\n         aumid=[%ls]\n", tag,
            id.exePath.empty() ? L"(none)" : id.exePath.c_str(),
            id.aumid.empty()   ? L"(none)" : id.aumid.c_str());
    fflush(stdout);
}

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? atoi(argv[1]) : 25;

    Print(L"INITIAL", ForegroundWatcher::Current());
    wprintf(L"Watching for %d seconds — switch windows now.\n", seconds);
    fflush(stdout);

    ForegroundWatcher watcher;
    if (!watcher.Start([](const ForegroundIdentity& id) { Print(L"CHANGED", id); })) {
        wprintf(L"Failed to install the foreground hook.\n");
        return 1;
    }

    // The hook is out-of-context, so its callback arrives through this
    // thread's message queue — without a pump nothing is ever delivered.
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    MSG msg;
    while (GetTickCount64() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
    return 0;
}
