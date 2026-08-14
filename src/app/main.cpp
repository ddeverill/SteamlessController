#include "TrayApp.h"
#include <Windows.h>
#include <clocale>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // The event log formats wide strings with %ls, which fprintf converts
    // through the C locale — and the default one encodes nothing above ASCII.
    // It does not fail loudly either: it abandons the line at the first
    // character it cannot encode, so a log line silently loses everything from
    // there on. Measured: a bus report ending in an em dash wrote its opening
    // clause and stopped mid-sentence.
    //
    // Which matters because the values most likely to carry non-ASCII are the
    // ones worth reading — a game's name, a foreground window, the process
    // holding the controller, a device's friendly name on a localised Windows.
    // LC_CTYPE alone, so number and date formatting keep the C locale's
    // behaviour, and here at the top so it lands before the first log line and
    // before any thread exists to race it.
    setlocale(LC_CTYPE, ".UTF8");

    // Enable per-monitor v2 DPI awareness so the WebView2 window renders crisp
    // on high-DPI displays and the WM_NCHITTEST pixel math stays correct.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Prevent multiple instances.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"SteamlessController_SingleInstance");
    if (!mutex) return 0;
    const DWORD wait = WaitForSingleObject(mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return 0;
    }

    TrayApp app;
    int result = 0;
    if (app.Init(hInstance))
        result = app.Run();

    CloseHandle(mutex);
    return result;
}
