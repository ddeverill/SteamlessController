#pragma once
#include <Windows.h>
#include <memory>

class ControllerManager;

class TrayApp {
public:
    TrayApp();
    ~TrayApp();

    bool Init(HINSTANCE hInstance);
    int  Run();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void AddTrayIcon();
    void RemoveTrayIcon();
    void UpdateTrayIcon(bool connected, bool gameModeActive, bool vigemMissing = false);
    void ShowViGEmBalloon();
    void ShowContextMenu();
    void LoadSettings();
    void SaveSettings();
    bool IsStartupEnabled() const;
    void SetStartupEnabled(bool enabled);

    HWND                               m_hwnd      = nullptr;
    HINSTANCE                          m_hInstance = nullptr;
    UINT                               m_wmTaskbar = 0;
    HICON                              m_iconOff   = nullptr;
    HICON                              m_iconOn    = nullptr;
    std::unique_ptr<ControllerManager> m_controller;

    static constexpr UINT IDM_TOGGLE        = 1001;
    static constexpr UINT IDM_EXIT          = 1002;
    static constexpr UINT IDM_TRACKPAD      = 1003;
    static constexpr UINT IDM_BACKBUTTONS   = 1004;
    static constexpr UINT IDM_LEFT_TRACKPAD = 1005;
    static constexpr UINT IDM_STARTUP       = 1006;
    static constexpr UINT WM_TRAY          = WM_APP + 1;
    static constexpr UINT TRAY_UID         = 1;
};
