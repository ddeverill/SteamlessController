#pragma once
#include <Windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <functional>
#include <map>
#include <vector>
#include "BackButtonConfig.h"
#include "ControllerPlatform.h"
#include "GameLibrary.h"
#include "TrackpadConfig.h"

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
    // games populates the per-game picker; gameProfiles supplies whatever
    // overrides already exist for entries in it. applyCallback fires on the
    // UI thread when the user clicks Apply — the game id identifies whichever
    // game was selected, or is empty for the default profile.
    // deleteCallback fires when the user removes a game's profile; its id is
    // one that was in gameProfiles, and never empty — the default profile
    // cannot be removed.
    void Open(HINSTANCE hInst, ControllerManager* mgr, const ControllerProfile& profile,
              std::vector<InstalledGame> games,
              std::map<std::wstring, ControllerProfile> gameProfiles,
              std::function<void(const std::wstring&, const ControllerProfile&)> applyCallback,
              std::function<void(const std::wstring&)> deleteCallback);

    void BringToFront() const;
    // Open in the sense that matters to callers: visible and being edited.
    // A window that has been closed is only hidden, not destroyed, so the
    // handle alone does not answer this.
    bool IsOpen()       const { return m_hwnd != nullptr && IsWindowVisible(m_hwnd); }

    // Fires when the window is dismissed. Set once and kept for the object's
    // lifetime — profile switching is suppressed while the window is up, so
    // somebody has to re-evaluate once it comes down.
    void SetOnClose(std::function<void()> fn) { m_onClose = std::move(fn); }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void CreateWebViewAsync(HWND hwnd);
    void OnControllerReady(ICoreWebView2Controller* ctrl);
    void OnWebMessage(const std::wstring& raw);
    void PostToWebView(const std::wstring& jsonStr);
    void SendInitState();

    // Tells the page a binding was captured. The page applies it to whichever
    // row is listening — it owns that state, not us.
    void PostCapturedBinding(const BackButtonBinding& binding);

    // Called from the read thread via PostMessage — marshals a captured button
    // back to the UI thread so we can call PostWebMessageAsString safely.
    static constexpr UINT WM_BUTTON_CAPTURED = WM_APP + 1;
    // The page has resolved its unsaved-changes prompt and the window may now
    // be hidden. Distinct from WM_CLOSE so the confirmed close does not loop
    // back into asking the page again.
    static constexpr UINT WM_CLOSE_CONFIRMED = WM_APP + 2;

    HWND              m_hwnd     = nullptr;
    HINSTANCE         m_hInst   = nullptr;
    ControllerManager* m_mgr    = nullptr;
    ControllerProfile m_config;
    std::vector<InstalledGame>                 m_games;
    std::map<std::wstring, ControllerProfile>  m_gameProfiles;
    std::function<void(const std::wstring&, const ControllerProfile&)> m_applyCallback;
    std::function<void(const std::wstring&)> m_deleteCallback;
    std::function<void()> m_onClose;

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
