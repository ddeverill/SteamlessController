#pragma once
#include <Windows.h>
#include <commctrl.h>
#include <memory>
#include <mutex>
#include <string>
#include "RemapWindow.h"
#include "SteamStrategy.h"
#include "SteamWatcher.h"

class ControllerManager;

// Who decides when SteamlessController takes over the physical controller.
enum class AutoMode {
    Manual        = 0,  // tray toggle only
    OffWhileSteam = 1,  // yield whenever steam.exe is running
    OffOnlyInGame = 2,  // yield only while a game is running (needs admin to
                        // wrest the device from a running Steam)
};

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
    void ShowAlertBalloon(const std::wstring& title, const std::wstring& text);
    void OpenEventLog();
    void ShowContextMenu();
    void LoadSettings();
    void SaveSettings();
    void OpenRemapWindow();
    void ApplySteamStrategy(SteamStrategy strategy);
    void CreateMenuTooltip();
    void ShowMenuTooltip(UINT commandId);
    void HideMenuTooltip();

    // The tray app never runs elevated (an elevated foreground window blocks
    // unelevated SendInput — Steam Input's desktop cursor — via UIPI).
    // Privileged work (cycling the device node) is delegated to the
    // SteamlessDeviceCycle helper, registered as an on-demand highest-
    // privileges scheduled task by the installer or, failing that, by a
    // one-time UAC prompt here.
    static bool IsProcessElevated();
    bool EnsureCycleTaskRegistered();
    bool RunKeyExists() const;
    void SetRunKey(bool enabled);
    void DeleteLegacyStartupTask();
    void UpdateStartupRegistration();

    // Auto-mode plumbing.
    void SetAutoMode(AutoMode mode);
    bool WantControl(SteamState state) const;
    void ApplySteamState(SteamState state);
    void TryAcquireController();
    bool RestartControllerDevices();
    void ShowElevationBalloon();

    HWND                               m_hwnd       = nullptr;
    HINSTANCE                          m_hInstance  = nullptr;
    UINT                               m_wmTaskbar  = 0;
    HICON                              m_iconOff    = nullptr;
    HICON                              m_iconOn     = nullptr;
    AutoMode                           m_autoMode   = AutoMode::OffWhileSteam;
    SteamStrategy                      m_steamStrategy = SteamStrategy::YieldToSteam;
    HWND                               m_menuTooltip = nullptr;
    TTTOOLINFOW                        m_menuTooltipInfo{};
    std::wstring                       m_menuTooltipText;
    int                                m_acquireRetries = 0;
    bool                               m_elevationBalloonShown = false;
    bool                               m_startupEnabled   = false;
    int                                m_startupMechanism = 0;  // 0 none, 1 Run key, 2 elevated task
    bool                               m_notificationsEnabled = true;  // disconnect/stall balloons
    std::mutex                         m_alertMutex;   // guards the two alert strings
    std::wstring                       m_alertTitle;   // set on read threads,
    std::wstring                       m_alertText;    // consumed on WM_ALERT
    std::unique_ptr<ControllerManager> m_controller;
    SteamWatcher                       m_steamWatcher;
    RemapWindow                        m_remapWindow;

    static constexpr UINT IDM_TOGGLE        = 1001;
    static constexpr UINT IDM_EXIT          = 1002;
    static constexpr UINT IDM_TRACKPAD      = 1003;
    static constexpr UINT IDM_REMAP_BACK    = 1004;
    static constexpr UINT IDM_LEFT_TRACKPAD = 1005;
    static constexpr UINT IDM_STARTUP       = 1006;
    static constexpr UINT IDM_BACKBUTTONS   = 1007;
    static constexpr UINT IDM_PLATFORM_XBOX = 1008;
    static constexpr UINT IDM_PLATFORM_PS   = 1009;
    static constexpr UINT IDM_MODE_MANUAL   = 1010;
    static constexpr UINT IDM_MODE_STEAM    = 1011;
    static constexpr UINT IDM_MODE_GAME     = 1012;
    static constexpr UINT IDM_OPENLOG       = 1013;
    static constexpr UINT IDM_NOTIFICATIONS = 1014;
    static constexpr UINT IDM_STEAM_YIELD    = 1020;
    static constexpr UINT IDM_STEAM_BLACKLIST = 1021;
    static constexpr UINT IDM_STEAM_NOJOY    = 1022;
    static constexpr UINT WM_TRAY           = WM_APP + 1;
    static constexpr UINT WM_STEAMSTATE     = WM_APP + 2;
    static constexpr UINT WM_ALERT          = WM_APP + 3;
    static constexpr UINT TRAY_UID          = 1;
    static constexpr UINT_PTR IDT_ACQUIRE   = 1;
    static constexpr int  MAX_ACQUIRE_CYCLES = 3;
};
