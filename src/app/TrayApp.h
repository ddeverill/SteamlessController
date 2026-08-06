#pragma once
#include <Windows.h>
#include <memory>
#include <mutex>
#include <string>
#include "DeviceRestart.h"
#include "RemapWindow.h"
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

    // Control plumbing, shared by every mode.
    void SetAutoMode(AutoMode mode);
    bool WantControl(SteamState state) const;
    void ApplySteamState(SteamState state);
    void TryAcquireController(uint32_t stateWaitMs = 250);
    void ReleaseControl();
    bool RestartControllerDevices();
    bool RefreshCycleStatus();
    bool LastCycleBrokeNothing();
    void ReportAcquireFailure();
    void ShowElevationBalloon();

    HWND                               m_hwnd       = nullptr;
    HINSTANCE                          m_hInstance  = nullptr;
    UINT                               m_wmTaskbar  = 0;
    HICON                              m_iconOff    = nullptr;
    HICON                              m_iconOn     = nullptr;
    AutoMode                           m_autoMode   = AutoMode::Manual;
    // Do we want the controller right now? Acquiring is asynchronous — a
    // blocked claim escalates to a device cycle that lands seconds later as a
    // WM_DEVICECHANGE — so intent has to outlive the call that started it.
    // Auto modes derive this from Steam's state; manual mode from the tray
    // toggle, which is otherwise nowhere on record.
    bool                               m_wantControl    = false;
    int                                m_acquireRetries = 0;
    ULONGLONG                          m_lastCycleTick  = 0;
    // Verdict of the most recent device cycle, read back from the helper.
    DeviceRestart::CycleResult         m_lastCycleStatus;
    bool                               m_elevationBalloonShown = false;
    bool                               m_vigemBalloonShown     = false;
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
    static constexpr UINT IDM_PLATFORM_XBOX = 1008;
    static constexpr UINT IDM_PLATFORM_PS   = 1009;
    static constexpr UINT IDM_MODE_MANUAL   = 1010;
    static constexpr UINT IDM_MODE_STEAM    = 1011;
    static constexpr UINT IDM_MODE_GAME     = 1012;
    static constexpr UINT IDM_OPENLOG       = 1013;
    static constexpr UINT IDM_NOTIFICATIONS = 1014;
    static constexpr UINT WM_TRAY           = WM_APP + 1;
    static constexpr UINT WM_STEAMSTATE     = WM_APP + 2;
    static constexpr UINT WM_ALERT          = WM_APP + 3;
    static constexpr UINT TRAY_UID          = 1;
    static constexpr UINT_PTR IDT_ACQUIRE         = 1;
    static constexpr UINT_PTR IDT_ACQUIRE_VERDICT = 2;
    static constexpr UINT_PTR IDT_WAKE_POLL       = 3;
    // A multi-slot receiver publishes every slot interface permanently, so a
    // controller waking up produces no device-change event — the only way to
    // notice is to keep asking. The probe is short because a live slot streams
    // continuously and answers within a few reports.
    static constexpr UINT WAKE_POLL_MS  = 2000;
    static constexpr UINT WAKE_PROBE_MS = 80;
    static constexpr int  MAX_ACQUIRE_CYCLES = 3;
    // Must outlast a full device cycle: the helper waits a second between
    // disable and enable, then a multi-slot receiver re-enumerates every
    // interface. At 2500 the app declared failure while the last cycle's
    // arrival was still in flight.
    static constexpr UINT ACQUIRE_RETRY_MS   = 4000;
    // Grace after the final cycle before reporting failure to the user.
    static constexpr UINT ACQUIRE_VERDICT_MS = 3000;
    // Minimum spacing between device cycles. A cycle is asynchronous, so
    // without this the arrivals it generates re-enter the acquire path and
    // fire another one on top of it.
    static constexpr ULONGLONG CYCLE_MIN_GAP_MS = 4000;
};
