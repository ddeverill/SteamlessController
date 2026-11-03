// RemapWindowProbe — opens the real Customize window, with no controller and no
// tray, and saves a picture of it plus what the page reports about its fonts. The
// page is a string handed to WebView2, so how it really renders (the bundled fonts
// in particular, which arrive through a request handler) can only be seen by
// running it in WebView2 itself.
//
//   RemapWindowProbe <out.bmp> [section]
#define NOMINMAX
#include <Windows.h>
#include <objbase.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "app/RemapWindow.h"

namespace {

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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: RemapWindowProbe <out.bmp>\n");
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    RemapWindow window;
    ControllerProfile profile;
    window.Open(GetModuleHandleW(nullptr), nullptr, profile, {},
                [](const std::wstring&, const ControllerProfile&) {},
                [](const std::wstring&) {});
    // WebView2 starts asynchronously and the page then asks for its fonts; give
    // both time, with the message loop running so the callbacks can land.
    Pump(6000);

    HWND hwnd = FindWindowW(L"SteamlessRemapWindow", nullptr);
    if (!hwnd) { std::printf("window did not open\n"); return 1; }
    SetForegroundWindow(hwnd);
    Pump(500);

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    std::wstring out(argv[1], argv[1] + std::strlen(argv[1]));
    const bool ok = SaveBmp(out.c_str(), rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
    std::printf("window %dx%d at %d,%d -> %s\n", rc.right - rc.left, rc.bottom - rc.top, rc.left, rc.top,
                ok ? "saved" : "save failed");
    DestroyWindow(hwnd);
    Pump(200);
    return ok ? 0 : 1;
}
