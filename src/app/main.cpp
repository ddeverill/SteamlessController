#include "TrayApp.h"
#include <Windows.h>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // Enable per-monitor v2 DPI awareness so the WebView2 window renders crisp
    // on high-DPI displays and the WM_NCHITTEST pixel math stays correct.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Prevent multiple instances.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"SteamlessController_SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    TrayApp app;
    int result = 0;
    if (app.Init(hInstance))
        result = app.Run();

    CloseHandle(mutex);
    return result;
}
