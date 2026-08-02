#pragma once
#include <Windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <functional>
#include "BackButtonConfig.h"

class ControllerManager;

class RemapWindow {
public:
    RemapWindow() = default;
    ~RemapWindow();
    RemapWindow(const RemapWindow&) = delete;
    RemapWindow& operator=(const RemapWindow&) = delete;

    // Opens (or re-focuses) the remap window.
    // Shows a "not supported" message box and returns without opening if WebView2
    // is unavailable (Windows 7/8 or no runtime installed).
    // applyCallback fires on the UI thread when the user clicks Apply.
    void Open(HINSTANCE hInst, ControllerManager* mgr, const BackButtonConfig& cfg,
              std::function<void(const BackButtonConfig&)> applyCallback);

    void BringToFront() const;
    bool IsOpen()       const { return m_hwnd != nullptr; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void CreateWebViewAsync(HWND hwnd);
    void OnControllerReady(ICoreWebView2Controller* ctrl);
    void OnWebMessage(const std::wstring& raw);
    void PostToWebView(const std::wstring& jsonStr);
    void SendInitState();

    // Called from the read thread via PostMessage — marshals a captured button
    // back to the UI thread so we can call PostWebMessageAsString safely.
    static constexpr UINT WM_BUTTON_CAPTURED = WM_APP + 1;

    HWND              m_hwnd     = nullptr;
    HINSTANCE         m_hInst   = nullptr;
    ControllerManager* m_mgr    = nullptr;
    BackButtonConfig  m_config;
    std::function<void(const BackButtonConfig&)> m_applyCallback;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_env;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller>  m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2>            m_webview;

    static RemapWindow* s_instance;

    static constexpr wchar_t CLASS_NAME[] = L"SteamlessRemapWindow";
    // Design sizes in logical (96-DPI) pixels — scaled to the monitor's DPI
    // at creation. The process is PER_MONITOR_AWARE_V2, so nothing scales
    // these for us.
    static constexpr int WINDOW_W = 760;
    static constexpr int WINDOW_H = 668; // 46 titlebar + ~560 body + 62 footer
    static constexpr int MIN_WINDOW_W = 560;
    static constexpr int MIN_WINDOW_H = 440;
};
