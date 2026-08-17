#pragma once
#include <Windows.h>
#include <memory>
#include <mutex>
#include <string>
#include "DeviceRestart.h"
#include "ForegroundWatcher.h"
#include "GameProfiles.h"
#include "RemapWindow.h"
#include "SteamAppLocator.h"
#include "SteamWatcher.h"

class ControllerManager;

// Who decides when SteamlessController takes over the physical controller.
enum class AutoMode {
    Manual        = 0,  // tray toggle only
    OffWhileSteam = 1,  // yield whenever steam.exe is running
    OffOnlyInGame = 2,  // yield only while a game is running (needs admin to
                        // wrest the device from a running Steam)
    // Hold the controller only while a game the user has made a profile for is
    // in front, and leave it alone the rest of the time.
    //
    // The one mode where a profile outranks yielding to Steam: the other two
    // ask "is Steam busy", and this one asks "did the user say they want us
    // driving this game". Having made a profile for it is that answer, so a
    // running Steam is not a reason to refuse — which is the only way a Steam
    // game launched from the Steam client can ever use one of our profiles.
    // Needs the same elevated cycle helper OffOnlyInGame does, for the same
    // reason: taking the device from a live Steam.
    //
    // Deliberately keyed on a profile existing rather than on the app being a
    // game, because "is this a game" is the harder question and this mode is
    // the place it will eventually be answered — widening the test is all it
    // takes to become "off unless in a game".
    OffUnlessProfile = 3,
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
    void UpdateTrayIcon(bool connected, bool gameModeActive, bool sharedHandle,
                        bool padUnavailable = false);
    void ShowViGEmBalloon();

    // What clicking the balloon should do. Balloons are transient and the click
    // arrives long after the call that raised one, so the action travels with
    // it rather than being inferred at click time.
    enum class BalloonAction { ViGEmDownload, EnableDisabledDevice, None };
    // infoFlags picks the icon Windows draws: a warning by default, because
    // most of these report something the user has to act on. Purely
    // informational notices pass NIIF_INFO and BalloonAction::None.
    void ShowAlertBalloon(const std::wstring& title, const std::wstring& text,
                          BalloonAction action = BalloonAction::ViGEmDownload,
                          DWORD infoFlags = NIIF_WARNING);
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

    // Per-game profile switching. The foreground application decides which
    // profile is live; see ForegroundWatcher for why focus rather than
    // "is it running" is the question being asked.
    void OnForegroundChanged(const ForegroundIdentity& id);
    // The profile id matching an application, or empty when none does.
    std::wstring MatchProfile(const ForegroundIdentity& id) const;
    // Record `gameId`'s profile as the selected one (empty selects the
    // default), and nothing else. Returns whether the selection changed.
    //
    // Deliberately separate from applying it: whether we want the controller
    // at all now depends on which profile is live, so the selection has to be
    // on record before EvaluateControl can read it. Keeping the two joined is
    // what made ReleaseControl and profile switching call each other.
    bool SelectProfile(const std::wstring& gameId);
    // The selected profile — a game's if one is selected, else the default.
    const ControllerProfile& ActiveProfile() const;
    // Push the selected profile's bindings into ControllerManager. Safe while
    // the controller is not ours, and called before acquiring rather than
    // after so the pad comes up already carrying the right bindings.
    void PushActiveProfile();
    // Re-resolve the foreground now, for the moments where the answer can
    // have changed while nobody was listening — taking the controller back,
    // or closing the window that suppresses switching.
    void RefreshActiveProfile();
    // Re-read Steam's install manifests. Driven by events rather than a
    // timer: startup, and whenever the user opens the window where profiles
    // are created. A game installed after that is invisible until one of
    // those happens again, which is the price of not polling the disk.
    void RefreshSteamApps();

    // Control plumbing, shared by every mode.
    void SetAutoMode(AutoMode mode);
    // Whether Steam's current state leaves the controller available to us.
    // A rule rather than a preference: no profile setting may vote itself
    // into driving the pad alongside Steam Input, which is worse than not
    // running at all (see ControllerManager's shared-handle note).
    bool SteamAllowsControl(SteamState state) const;
    // Do we want the controller right now? One question per mode — the tray
    // toggle, Steam's state, or whether a game profile is in front — rather
    // than one answer assembled from settings that override each other.
    bool WantControlNow() const;
    // Act on that answer — acquire, or schedule a release. The one place that
    // turns intent into an acquire or a release, so every caller that can
    // change the answer routes through here.
    void EvaluateControl();
    void ApplySteamState(SteamState state);
    void TryAcquireController(uint32_t stateWaitMs = 250);
    void ReleaseControl();
    void RecoverStrandedDevices();
    void WriteHeartbeat();
    // Carry out whatever the pending-disable record asks for: in process when
    // elevated, through the helper's task otherwise. Shared so there is one
    // place that knows how to get a devnode switched back on.
    bool RunPendingRecovery();
    // User-initiated repair of a devnode this app did not disable.
    void EnableDisabledControllerDevice();
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
    HICON                              m_iconShared = nullptr;
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
    // Latched for the lifetime of one attempt, and deliberately not cleared by
    // an ordinary state notification: the acquire path notifies several times
    // per attempt, and clearing it there let a single attempt raise a balloon
    // on every pass. Reported from the field as toasts Windows was still
    // replaying minutes after the app had been closed (#68).
    bool                               m_vigemBalloonShown     = false;
    bool                               m_startupEnabled   = false;
    int                                m_startupMechanism = 0;  // 0 none, 1 Run key, 2 elevated task
    // Every balloon this app raises, not just the disconnect and stall ones
    // it started out covering — a per-game profile loading is announced
    // through it too.
    bool                               m_notificationsEnabled = true;
    BalloonAction                      m_balloonAction = BalloonAction::ViGEmDownload;
    // Unbiased interrupt time at the last heartbeat, and the tick this instance
    // started. Unbiased so that time the machine spent asleep does not read as
    // the app having been blocked.
    ULONGLONG                          m_lastHeartbeatUnbiased = 0;
    ULONGLONG                          m_startTick = 0;
    // A cycle we fired is still running. While set, the device is deliberately
    // left unopened: our own handle vetoes the cycle just as readily as anyone
    // else's. Cleared by the arrival that ends the cycle, or by the tick
    // backstop when no arrival comes.
    bool                               m_cycleInFlight = false;
    bool                               m_inFlightLogged = false;  // one line per cycle
    // Devnode found disabled when the menu was last built, so the command
    // handler and the menu agree on what "re-enable" refers to.
    std::wstring                       m_disabledDeviceNode;
    std::mutex                         m_alertMutex;   // guards the two alert strings
    std::wstring                       m_alertTitle;   // set on read threads,
    std::wstring                       m_alertText;    // consumed on WM_ALERT
    std::unique_ptr<ControllerManager> m_controller;
    SteamWatcher                       m_steamWatcher;
    ForegroundWatcher                  m_foregroundWatcher;
    // Bridges a Steam profile's app id to the running executable. Refreshed
    // at the few moments the answer can have changed — see RefreshSteamApps.
    SteamAppLocator                    m_steamApps;
    RemapWindow                        m_remapWindow;
    // Per-game overrides, keyed by GameLibrary's opaque game id.
    // Most users never populate this; empty by default.
    std::map<std::wstring, ControllerProfile> m_gameProfiles;
    // The user's own settings, and the only profile that is ever persisted to
    // the app's registry key. Deliberately not the same thing as whatever
    // ControllerManager is running: while a game profile is active that is a
    // per-game override, and writing it back would quietly replace the user's
    // defaults with some game's bindings.
    ControllerProfile                  m_defaultProfile;
    // Which game's profile is live, or empty for the default. The key into
    // m_gameProfiles, so it is also the answer to "did the active profile
    // actually change" without comparing whole profiles.
    std::wstring                       m_activeGameId;
    // The game the "profile loaded" balloon last named, so alt-tabbing in and
    // out of one game does not raise it over and over.
    std::wstring                       m_toastedGameId;

    static constexpr UINT IDM_TOGGLE        = 1001;
    static constexpr UINT IDM_EXIT          = 1002;
    // 1003 and 1005 were the retired "Enable Trackpad Mouse" and "Use Left
    // Trackpad Instead" items — left unused rather than reassigned, so a
    // stale menu message from an old build can't land on a new command.
    static constexpr UINT IDM_REMAP_BACK    = 1004;
    static constexpr UINT IDM_STARTUP       = 1006;
    // 1008 and 1009 were the retired "Controller Platform" submenu items,
    // now a dropdown in the customization window. Left unused rather than
    // reassigned, like 1003/1005 above.
    static constexpr UINT IDM_MODE_MANUAL   = 1010;
    static constexpr UINT IDM_MODE_STEAM    = 1011;
    static constexpr UINT IDM_MODE_GAME     = 1012;
    static constexpr UINT IDM_OPENLOG       = 1013;
    static constexpr UINT IDM_NOTIFICATIONS = 1014;
    static constexpr UINT IDM_ENABLE_DEVICE = 1015;
    static constexpr UINT IDM_MODE_PROFILE  = 1016;
    static constexpr UINT WM_TRAY           = WM_APP + 1;
    static constexpr UINT WM_STEAMSTATE     = WM_APP + 2;
    static constexpr UINT WM_ALERT          = WM_APP + 3;
    static constexpr UINT TRAY_UID          = 1;
    static constexpr UINT_PTR IDT_ACQUIRE         = 1;
    static constexpr UINT_PTR IDT_ACQUIRE_VERDICT = 2;
    static constexpr UINT_PTR IDT_WAKE_POLL       = 3;
    static constexpr UINT_PTR IDT_HEARTBEAT       = 4;
    static constexpr UINT_PTR IDT_RELEASE_GRACE   = 5;
    // Long enough that idling for a month costs a fraction of the log's 512 KB,
    // short enough to bound when the app stopped responding to within a
    // quarter hour. Resolution only has to beat "somewhere in the last 33
    // hours", which is what the log offered the last time this mattered.
    static constexpr UINT HEARTBEAT_MS = 15 * 60 * 1000;
    // Slack before a late beat is called a stall. Window timers are low
    // priority and coalesced, so a beat is routinely a little late; a full
    // minute of drift is not.
    static constexpr UINT HEARTBEAT_SLACK_MS = 60 * 1000;
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
    // Retry spacing when the controller is fine but no virtual pad can be
    // created. Nothing here is a race, so the fast cadence above buys nothing:
    // what it is waiting for is a human installing a driver. Slow enough that
    // waiting through the whole thing costs a handful of log lines, quick
    // enough that the app picks a new driver up on its own.
    static constexpr UINT VIGEM_RETRY_MS = 30000;
    // Grace before a profile-driven release actually happens. Quick to engage
    // and slow to disengage, the same asymmetry SteamWatcher uses for Steam
    // going away — because each release and re-acquire round trip unplugs and
    // replugs the virtual pad, which a running game sees as the controller
    // being yanked out. Alt-tabbing to a browser mid-game is common enough
    // that doing it immediately would read as a bug. Steam turning up is the
    // one release that skips this; see EvaluateControl.
    static constexpr UINT RELEASE_GRACE_MS = 5000;
    // Minimum spacing between device cycles. A cycle is asynchronous, so
    // without this the arrivals it generates re-enter the acquire path and
    // fire another one on top of it.
    static constexpr ULONGLONG CYCLE_MIN_GAP_MS = 4000;
};
