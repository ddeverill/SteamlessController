// TrayPanelProbe — opens the tray panel in a chosen state and saves a picture of
// it, so the panel's look can be checked without a controller or a tray click.
//
//   TrayPanelProbe <state> <out.bmp>
//
// States: manual-on, manual-off, auto, nocontroller, devicedisabled.
// Add "hover" as a third argument to park the pointer on Customize Controls.
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "app/TrayPanel.h"

namespace {

TrayPanel::Model ModelFor(const char* state) {
    TrayPanel::Model m;
    m.connected = true;
    m.manual = true;
    m.controlOn = true;
    m.battery = 82;
    m.startup = true;
    m.enableOnLaunch = true;
    m.notifications = true;
    if (!strcmp(state, "manual-off")) {
        m.controlOn = false;
    } else if (!strcmp(state, "auto")) {
        m.manual = false;
        m.mode = 1;
        m.battery = 64;
        m.enableOnLaunch = false;
    } else if (!strcmp(state, "nocontroller")) {
        m.connected = false;
        m.controlOn = false;
        m.battery = -1;
    } else if (!strcmp(state, "devicedisabled")) {
        m.deviceDisabled = true;
        m.connected = false;
        m.controlOn = false;
        m.battery = -1;
    }
    return m;
}

void Pump(DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

bool SaveBmp(const wchar_t* path, int x, int y, int w, int h) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);

    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4d42;
    const DWORD size = static_cast<DWORD>(w) * static_cast<DWORD>(h) * 4;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + size;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = f != INVALID_HANDLE_VALUE;
    if (ok) {
        DWORD wr = 0;
        WriteFile(f, &fh, sizeof(fh), &wr, nullptr);
        WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &wr, nullptr);
        WriteFile(f, bits, size, &wr, nullptr);
        CloseHandle(f);
    }
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// Drives the real panel window with the messages a mouse and keyboard would
// send, and checks what comes out: which action fires, whether the panel stays
// up for the settings that should keep it there and goes for the ones that
// should not, and that the face follows the model.
int SelfTest() {
    std::printf("TrayPanel self-test\n");
    TrayPanel::Model model;
    model.connected = true; model.manual = true; model.controlOn = false;
    model.startup = true; model.enableOnLaunch = true; model.notifications = true;

    std::vector<TrayPanel::Action> seen;
    bool openInsideHandler = true;
    TrayPanel panel;
    auto show = [&] {
        panel.Show(GetModuleHandleW(nullptr), POINT{ 1500, 900 }, [&] { return model; },
                   [&](TrayPanel::Action a) {
                       seen.push_back(a);
                       openInsideHandler = panel.IsOpen();
                       if (a == TrayPanel::Action::ToggleControl) model.controlOn = !model.controlOn;
                       if (a == TrayPanel::Action::ModeGame) model.mode = 2;
                       if (a == TrayPanel::Action::ModeSteam) { model.mode = 1; model.manual = false; }
                       if (a == TrayPanel::Action::Startup) model.startup = !model.startup;
                   });
        Pump(150);
    };
    auto click = [&](float x, float y) {
        HWND h = FindWindowW(L"SteamlessTrayPanel", nullptr);
        const float s = static_cast<float>(GetDpiForWindow(h)) / 96.f;
        SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(static_cast<int>(x * s), static_cast<int>(y * s)));
    };
    auto key = [&](WPARAM vk) {
        HWND h = FindWindowW(L"SteamlessTrayPanel", nullptr);
        SendMessageW(h, WM_KEYDOWN, vk, 0);
    };

    show();
    Check(panel.IsOpen(), "opens");

    click(305, 34);   // master switch
    Check(!seen.empty() && seen.back() == TrayPanel::Action::ToggleControl, "master switch sends ToggleControl");
    Check(panel.IsOpen(), "panel stays open after a setting change");

    click(210, 107);  // "In game" segment
    Check(seen.back() == TrayPanel::Action::ModeGame, "In game segment sends ModeGame");
    const size_t before = seen.size();
    click(210, 107);  // already selected
    Check(seen.size() == before, "choosing the selected mode again does nothing");

    click(170, 205);  // Customize Controls
    Check(seen.back() == TrayPanel::Action::Customize, "Customize Controls sends Customize");
    Check(!panel.IsOpen(), "panel closes for Customize");
    Check(!openInsideHandler, "panel is already gone when the handler runs");

    // Leaving Manual removes the Enable on Launch row. The window must glide to
    // the shorter height with its bottom edge where it was, not jump.
    model.mode = 0; model.manual = true;
    show();
    {
        HWND h = FindWindowW(L"SteamlessTrayPanel", nullptr);
        RECT r0{};
        GetWindowRect(h, &r0);
        click(133, 107);  // "Steam" segment
        std::vector<int> heights;
        const ULONGLONG until = GetTickCount64() + 400;
        while (GetTickCount64() < until) {
            Pump(12);
            RECT r{};
            GetWindowRect(h, &r);
            heights.push_back(r.bottom - r.top);
            if (r.bottom != r0.bottom) { Check(false, "bottom edge stays put while animating"); break; }
        }
        bool monotonic = true, between = false;
        for (size_t i = 1; i < heights.size(); ++i) {
            if (heights[i] > heights[i - 1]) monotonic = false;
            if (heights[i] > 385 && heights[i] < 425) between = true;
        }
        Check(r0.bottom - r0.top == 425, "starts at the 425-high manual size");
        Check(between, "passes through heights between the two sizes");
        Check(monotonic, "height only ever shrinks");
        Check(heights.back() == 385, "settles at the 385-high auto size");
        panel.Close();
    }

    // Mode is now an auto mode, so Enable on Launch must have left the panel.
    model.mode = 2; model.manual = false;
    show();
    HWND hw = FindWindowW(L"SteamlessTrayPanel", nullptr);
    RECT rc{};
    GetWindowRect(hw, &rc);
    const float s = static_cast<float>(GetDpiForWindow(hw)) / 96.f;
    Check(static_cast<int>(std::lround(385 * s)) == rc.bottom - rc.top, "auto mode drops the Enable on Launch row (385 high)");

    key(VK_DOWN); key(VK_RETURN);
    Check(seen.back() != TrayPanel::Action::ToggleControl || !model.manual, "keyboard: Down then Enter activates the first enabled control");
    const auto first = seen.back();
    Check(first == TrayPanel::Action::ModeManual, "…which is the first mode segment, because the master switch is disabled under auto mode");

    key(VK_ESCAPE);
    Check(!panel.IsOpen(), "Esc closes");

    show();
    SendMessageW(FindWindowW(L"SteamlessTrayPanel", nullptr), WM_ACTIVATE, WA_INACTIVE, 0);
    Check(!panel.IsOpen(), "losing activation closes (click-away)");
    Check(panel.RecentlyDismissed(), "…and is remembered as a dismissal");
    Pump(450);
    Check(!panel.RecentlyDismissed(), "the dismissal is forgotten after the grace period");

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "selftest")) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        return SelfTest();
    }
    if (argc < 3) {
        std::printf("usage: TrayPanelProbe <manual-on|manual-off|auto|nocontroller|devicedisabled> <out.bmp> [hover]\n");
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const TrayPanel::Model model = ModelFor(argv[1]);
    const bool hover = argc > 3 && !strcmp(argv[3], "hover");

    TrayPanel panel;
    // Anchored toward the bottom-right of the primary monitor's work area, as a
    // tray click would be.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    POINT anchor{ work.right - 40, work.bottom };
    panel.Show(GetModuleHandleW(nullptr), anchor, [model] { return model; },
               [](TrayPanel::Action) {});
    Pump(500);

    HWND hwnd = FindWindowW(L"SteamlessTrayPanel", nullptr);
    if (!hwnd) { std::printf("panel did not open\n"); return 1; }

    if (hover) {
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        const float s = static_cast<float>(GetDpiForWindow(hwnd)) / 96.f;
        // Customize Controls sits just under the mode section; aim at its middle.
        const int top = model.deviceDisabled ? 52 : 0;
        SetCursorPos(rc.left + static_cast<int>(170 * s), rc.top + static_cast<int>((top + 68 + 100 + 1 + 36) * s));
        Pump(300);
    }

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    const int pad = 32;
    const int x = rc.left - pad, y = rc.top - pad;
    const int w = (rc.right - rc.left) + pad * 2, h = (rc.bottom - rc.top) + pad * 2;
    std::wstring out(argv[2], argv[2] + std::strlen(argv[2]));
    const bool ok = SaveBmp(out.c_str(), x, y, w, h);
    std::printf("panel %dx%d at %d,%d -> %s\n", rc.right - rc.left, rc.bottom - rc.top, rc.left, rc.top,
                ok ? "saved" : "save failed");
    panel.Close();
    return ok ? 0 : 1;
}
