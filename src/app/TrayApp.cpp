#include "TrayApp.h"
#include "ControllerManager.h"
#include "ControllerPlatform.h"
#include "DeviceRestart.h"
#include "EventLog.h"
#include "GameLibrary.h"
#include "GameProfiles.h"
#include "InputInjection.h"
#include "steam/SteamController.h"
#include "resource.h"
#include <shellapi.h>
#include <dbt.h>
#include <winreg.h>
#include <cstdio>
#include <cstring>
#include <string>

static TrayApp* g_app = nullptr;

static constexpr wchar_t WNDCLASS_NAME[] = L"SteamlessControllerTray";

static constexpr wchar_t CYCLE_TASK_NAME[]          = L"SteamlessControllerDeviceCycle";
static constexpr wchar_t LEGACY_STARTUP_TASK_NAME[] = L"SteamlessController";
static constexpr wchar_t HELPER_EXE_NAME[]          = L"SteamlessDeviceCycle.exe";

// Runs a console tool with no visible window; true when it exits 0.
static bool RunToolHidden(std::wstring cmdline) {
    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// Path of the helper exe, expected beside our own executable.
static std::wstring HelperPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe);
    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) path.resize(slash + 1);
    path += HELPER_EXE_NAME;
    return path;
}

TrayApp::TrayApp() {
    g_app = this;
}

TrayApp::~TrayApp() {
    // Stop the watchers before anything else so their callbacks can't post to
    // a window (or reference a controller) that is being torn down.
    m_steamWatcher.Stop();
    m_foregroundWatcher.Stop();
    RemoveTrayIcon();
    g_app = nullptr;
}

bool TrayApp::Init(HINSTANCE hInstance) {
    m_hInstance = hInstance;
    m_iconOff    = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_OFF));
    m_iconOn     = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_ON));
    m_iconShared = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_SHARED));
    m_wmTaskbar = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = WNDCLASS_NAME;
    if (!RegisterClassExW(&wc)) return false;

    // Message-only window — invisible, never shown.
    m_hwnd = CreateWindowExW(0, WNDCLASS_NAME, L"SteamlessController",
                             0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);
    if (!m_hwnd) return false;

    // Register for HID device arrival/removal notifications.
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size       = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid  = {0x4D1E55B2, 0xF16F, 0x11CF,
                              {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
    RegisterDeviceNotificationW(m_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    EventLog::Write("=== SteamlessController %s started ===", APP_VERSION_STR);

    m_startTick = GetTickCount64();
    QueryUnbiasedInterruptTime(&m_lastHeartbeatUnbiased);

    m_controller = std::make_unique<ControllerManager>(
        [this](bool connected, bool gameModeActive, bool sharedHandle, bool vigemMissing) {
            UpdateTrayIcon(connected, gameModeActive, sharedHandle, vigemMissing);
        });
    auto raiseAlert = [this](const std::wstring& title, const std::wstring& text) {
        {
            std::lock_guard<std::mutex> lk(m_alertMutex);
            m_alertTitle = title;
            m_alertText  = text;
        }
        PostMessageW(m_hwnd, WM_ALERT, 0, 0);
    };
    m_controller->SetAlertCallback(raiseAlert);
    // Blocked input reaches the user the same way a dead controller does: it
    // presents identically — nothing happens — and the cause is something only
    // this app is in a position to name.
    InputInjection::SetAlertCallback(raiseAlert);

    LoadSettings();
    // Converge the startup mechanism with the loaded mode — e.g. the first
    // elevated run after enabling in-game mode migrates the Run key to the
    // highest-privileges logon task (and back when the mode changes).
    UpdateStartupRegistration();
    AddTrayIcon();
    UpdateTrayIcon(m_controller->IsConnected(), m_controller->IsGameModeActive(),
                   m_controller->IsGameModeShared(), false);

    // The in-game mode needs the elevated cycle helper to take the controller
    // back from a running Steam. Settle that here rather than on first use:
    // the installer normally registers the task, but when it hasn't (running
    // from a build folder, or after an uninstall) the only other trigger is
    // the cycle itself — so the UAC prompt ambushed the user mid-game-launch.
    // A no-op once the task exists, which is the common case. Must run after
    // AddTrayIcon, since a declined prompt reports via the tray balloon.
    if (m_autoMode == AutoMode::OffOnlyInGame)
        EnsureCycleTaskRegistered();

    // Deliberately last, and every part of that ordering is load-bearing: it
    // reports through the tray balloon, which does nothing until AddTrayIcon has
    // run; it honours the notification setting, which LoadSettings supplies; and
    // it may need the elevated helper, so it goes after the task registration
    // above rather than raising a second UAC prompt of its own.
    //
    // Nothing earlier needs the controller to exist. A device brought back here
    // announces itself as an ordinary DBT_DEVICEARRIVAL once the message loop
    // starts, and is picked up on that path like any other replug.
    RecoverStrandedDevices();

    // Runs for the life of the process in every mode. Nothing here depends on
    // it, which is the point: it exists so that the log can tell "idle" from
    // "stopped" without anyone having to reproduce the fault.
    SetTimer(m_hwnd, IDT_HEARTBEAT, HEARTBEAT_MS, nullptr);

    // Start watching for Steam regardless of auto mode — the state is applied
    // only when auto mode is on, and having it warm makes toggling auto mode
    // take effect instantly. The initial callback asserts the correct state
    // at startup (which also self-heals after a previous crash).
    m_steamWatcher.Start([this](SteamState state) {
        PostMessageW(m_hwnd, WM_STEAMSTATE, static_cast<WPARAM>(state), 0);
    });

    // Nothing switches profiles while the window is up, so a profile created
    // for the game the user is already in has to be picked up when it closes.
    m_remapWindow.SetOnClose([this] { RefreshActiveProfile(); });

    // What Steam has installed, for matching a running game back to the app
    // id its profile is keyed by.
    RefreshSteamApps();

    // Per-game profiles. Started unconditionally for the same reason as the
    // Steam watcher: the callback decides whether anything applies, and a
    // watcher that is already warm makes the first switch instant. Costs one
    // hook and no thread — it only runs when the user changes applications.
    if (!m_foregroundWatcher.Start([this](const ForegroundIdentity& id) {
            OnForegroundChanged(id);
        }))
        EventLog::Write("PROFILE: foreground watcher could not start — "
                        "per-game profiles will not switch by themselves");

    // Resolve what is in front right now. The watcher above only reports
    // changes, so without this a user who launches us while already sitting
    // in a game would be judged against the default profile until they next
    // switched windows — and with a default that hands the controller back,
    // that means no controller.
    //
    // Selection only, no control decision: the first Steam state is still in
    // flight on the watcher thread, and deciding against the NoSteam it has
    // not confirmed yet would be guessing.
    SelectProfile(MatchProfile(ForegroundWatcher::Current()));
    PushActiveProfile();

    return true;
}

int TrayApp::Run() {
    MSG msg;
    BOOL ret;
    while ((ret = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (ret == -1) return -1;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK TrayApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app) return g_app->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayApp::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == m_wmTaskbar) {
        AddTrayIcon();
        return 0;
    }

    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == NIN_BALLOONUSERCLICK) {
            if (m_balloonAction == BalloonAction::EnableDisabledDevice)
                EnableDisabledControllerDevice();
            else if (m_balloonAction == BalloonAction::ViGEmDownload)
                ShellExecuteW(nullptr, L"open", L"https://github.com/nefarius/ViGEmBus/releases/latest",
                              nullptr, nullptr, SW_SHOWNORMAL);
            // BalloonAction::None: purely informational, nothing to open.
        }
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK)
            OpenRemapWindow();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP)
            ShowContextMenu();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_TOGGLE:
            // Only reachable in manual mode (grayed otherwise), so the click
            // is the sole source of intent — record it and take the same
            // acquire/release path the auto modes use. A second click while an
            // acquisition is still cycling cancels it rather than restarting.
            if (m_wantControl) {
                ReleaseControl();
            } else {
                // Taking the device back from a running Steam needs the
                // elevated helper. Settle the one-time UAC prompt on this
                // click instead of letting it ambush the first cycle.
                if (!IsProcessElevated())
                    EnsureCycleTaskRegistered();
                m_wantControl    = true;
                m_acquireRetries = 0;
                TryAcquireController();
            }
            break;
        case IDM_REMAP_BACK:
            OpenRemapWindow();
            break;
        case IDM_MODE_MANUAL:
            SetAutoMode(AutoMode::Manual);
            break;
        case IDM_MODE_STEAM:
            SetAutoMode(AutoMode::OffWhileSteam);
            break;
        case IDM_MODE_GAME:
            // Make sure the elevated helper task exists (one-time UAC when
            // the installer didn't register it) before the mode needs it.
            if (!IsProcessElevated())
                EnsureCycleTaskRegistered();
            SetAutoMode(AutoMode::OffOnlyInGame);
            break;
        case IDM_MODE_PROFILE:
            // Same reason as the mode above: this one can be asked to take the
            // controller while Steam is running it, and that needs the helper.
            if (!IsProcessElevated())
                EnsureCycleTaskRegistered();
            SetAutoMode(AutoMode::OffUnlessProfile);
            // With nothing to match, this mode holds the controller for
            // nothing at all. Said once, on the click, because a controller
            // that has gone quiet and stays quiet is otherwise indisting-
            // uishable from the app having broken.
            if (m_gameProfiles.empty() && m_notificationsEnabled)
                ShowAlertBalloon(L"No game profiles yet",
                                 L"The controller stays in its own keyboard/mouse mode "
                                 L"until you add a profile for a game in Customize Controls.",
                                 BalloonAction::None, NIIF_INFO);
            break;
        case IDM_STARTUP:
            m_startupEnabled = !m_startupEnabled;
            UpdateStartupRegistration();
            break;
        case IDM_OPENLOG:
            OpenEventLog();
            break;
        case IDM_NOTIFICATIONS:
            m_notificationsEnabled = !m_notificationsEnabled;
            EventLog::Write("USER: notifications %s",
                            m_notificationsEnabled ? "enabled" : "disabled");
            SaveSettings();
            break;
        case IDM_ENABLE_DEVICE:
            EnableDisabledControllerDevice();
            break;
        case IDM_EXIT:
            EventLog::Write("=== SteamlessController exiting (user request) ===");
            m_controller->DisableGameMode();
            PostQuitMessage(0);
            break;
        }
        return 0;

    case WM_STEAMSTATE: {
        const auto steamState = static_cast<SteamState>(wp);
        ApplySteamState(steamState);
        return 0;
    }

    case WM_ALERT: {
        if (!m_notificationsEnabled)
            return 0;  // the cause is still in the event log
        std::wstring title, text;
        {
            std::lock_guard<std::mutex> lk(m_alertMutex);
            title = m_alertTitle;
            text  = m_alertText;
        }
        if (!title.empty())
            ShowAlertBalloon(title, text);
        return 0;
    }

    case WM_TIMER:
        if (wp == IDT_ACQUIRE) {
            KillTimer(m_hwnd, IDT_ACQUIRE);
            if (m_wantControl)
                TryAcquireController();
        } else if (wp == IDT_WAKE_POLL) {
            KillTimer(m_hwnd, IDT_WAKE_POLL);
            // Cheap probe — we are only asking whether anything woke up.
            if (m_wantControl)
                TryAcquireController(WAKE_PROBE_MS);
        } else if (wp == IDT_RELEASE_GRACE) {
            KillTimer(m_hwnd, IDT_RELEASE_GRACE);
            // Re-asked rather than assumed: the grace period exists precisely
            // so the answer can change while it runs, and coming back to the
            // game is the case it is here for.
            if (!WantControlNow()) ReleaseControl();
        } else if (wp == IDT_HEARTBEAT) {
            // Periodic, so not killed — its absence is the signal.
            WriteHeartbeat();
        } else if (wp == IDT_ACQUIRE_VERDICT) {
            KillTimer(m_hwnd, IDT_ACQUIRE_VERDICT);
            // The last cycle's re-arrival had a grace period to land. If it
            // did, the controller is ours and there is nothing to report.
            if (m_controller->IsGameModeActive()) return 0;
            // Cycling is over — either the budget is spent or a cycle proved it
            // frees nothing. Exclusivity is not coming. Before calling it a
            // failure, take the controller on a shared handle: on machines where
            // the cycle never wins the reopen race that is the difference
            // between a working controller and none at all (#79). The tray shows
            // the shared icon so it is visible that Steam is still driving too.
            if (m_wantControl &&
                m_controller->EnableGameMode(250, /*allowShared=*/true)
                    == ControllerManager::GameModeOutcome::Enabled) {
                EventLog::Write("ACQUIRE: exclusivity unavailable after %d cycle(s) — "
                                "taking the controller on a shared handle",
                                m_acquireRetries);
                RefreshActiveProfile();
                return 0;
            }
            ReportAcquireFailure();
        }
        return 0;

    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
            m_controller->OnDeviceChange();
            // A controller arriving (plugged in, or re-arriving after a device
            // cycle) should be acquired straight away whenever control is
            // wanted — this is what wins the reopen race against Steam.
            if (wp == DBT_DEVICEARRIVAL && m_wantControl) {
                // Our controller specifically, not merely something HID-shaped:
                // the notification filter covers the whole HID class, so a mouse
                // being plugged in mid-cycle would otherwise clear the guard and
                // let us reopen the device we are busy cycling.
                if (m_controller->IsConnected())
                    m_cycleInFlight = false;
                TryAcquireController();
            }
        }
        return TRUE;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void TrayApp::SetAutoMode(AutoMode mode) {
    EventLog::Write("MODE: control mode set to %d "
                    "(0=manual 1=offWhileSteam 2=offOnlyInGame 3=offUnlessProfile)",
                    static_cast<int>(mode));
    m_autoMode = mode;
    m_acquireRetries = 0;
    SaveSettings();
    UpdateStartupRegistration();
    if (mode == AutoMode::Manual)
        // Manual takes its intent from the tray toggle. Adopt whatever is in
        // effect right now rather than acquiring or releasing behind the
        // user's back just because they changed mode.
        m_wantControl = m_controller->IsGameModeActive();
    else
        EvaluateControl();
}

// ---------------------------------------------------------------------------
// Per-game profiles
// ---------------------------------------------------------------------------

// Case-insensitive whole-path comparison. Both sides are already full Win32
// paths — one from the shortcut the profile was created from, the other from
// QueryFullProcessImageNameW — so no canonicalisation is attempted here; a
// game reached through a substituted drive or a symlink is a known gap.
static bool SamePath(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring TrayApp::MatchProfile(const ForegroundIdentity& id) const {
    static constexpr wchar_t kSteamPrefix[] = L"steam://rungameid/";
    static constexpr size_t  kSteamPrefixLen = ARRAYSIZE(kSteamPrefix) - 1;

    // Resolved once rather than per profile: the lookup walks every known
    // Steam install, and the answer cannot differ between profiles.
    const std::wstring steamAppId = m_steamApps.AppIdForPath(id.exePath);

    for (const auto& [profileId, profile] : m_gameProfiles) {
        if (profileId.rfind(L"aumid:", 0) == 0) {
            if (!id.aumid.empty()
                && _wcsicmp(profileId.c_str() + 6, id.aumid.c_str()) == 0)
                return profileId;
        } else if (profileId.rfind(kSteamPrefix, 0) == 0) {
            if (!steamAppId.empty() && profileId.compare(kSteamPrefixLen,
                                                         std::wstring::npos,
                                                         steamAppId) == 0)
                return profileId;
        } else if (SamePath(profileId, id.exePath)) {
            return profileId;
        }
    }
    return {};
}

void TrayApp::RefreshSteamApps() {
    m_steamApps.Refresh();
    EventLog::Write("PROFILE: %zu installed Steam app(s) located for profile matching",
                    m_steamApps.Count());
}

const ControllerProfile& TrayApp::ActiveProfile() const {
    if (!m_activeGameId.empty()) {
        auto it = m_gameProfiles.find(m_activeGameId);
        // A profile that follows the default resolves to it here rather than
        // being copied anywhere, so editing the default reaches every game
        // following it without touching a single stored profile.
        if (it != m_gameProfiles.end() && !it->second.useDefaultMappings)
            return it->second;
    }
    return m_defaultProfile;
}

bool TrayApp::SelectProfile(const std::wstring& gameId) {
    if (gameId == m_activeGameId) return false;

    // A profile deleted between resolving and selecting leaves nothing to
    // switch to; the default is the right answer, not a stale id.
    if (!gameId.empty() && m_gameProfiles.find(gameId) == m_gameProfiles.end())
        return SelectProfile({});

    m_activeGameId = gameId;
    EventLog::Write("PROFILE: switched to %ls",
                    gameId.empty() ? L"the default profile" : gameId.c_str());
    return true;
}

void TrayApp::PushActiveProfile() {
    const ControllerProfile& profile = ActiveProfile();
    m_controller->SetProfile(profile);

    if (m_activeGameId.empty()) return;  // returning to the default is not news

    // Announced only when the profile is actually going to run. Pushing
    // happens before the control decision, so without this the balloon would
    // promise a game's custom controls at the same moment we hand the
    // controller back and provide none.
    if (!WantControlNow()) return;

    // Told once per game rather than on every activation. Alt-tabbing out of
    // a game and back in reloads its profile, and a balloon on each round
    // trip would be noise — the point is to confirm the profile was found at
    // all, which the first one does.
    if (m_activeGameId == m_toastedGameId) return;
    m_toastedGameId = m_activeGameId;

    if (!m_notificationsEnabled) return;
    // Falls back to the id when the profile predates display names being
    // stored; an exe path is poor prose but it still identifies the game.
    const std::wstring& name = profile.displayName.empty() ? m_activeGameId
                                                           : profile.displayName;
    ShowAlertBalloon(L"Custom controls loaded",
                     L"Custom controller profile for " + name + L" loaded.",
                     BalloonAction::None, NIIF_INFO);
}

void TrayApp::OnForegroundChanged(const ForegroundIdentity& id) {
    // Deliberately not gated on the controller already being ours. With a
    // default profile that can hand the controller back, this is the path
    // that decides whether we hold the device at all, so it has to keep
    // running precisely when we do not — otherwise nothing would ever notice
    // the game that is supposed to take it back.
    //
    // The customization window edits one profile while another could be
    // running underneath it. Switching out from under the user mid-edit
    // would apply their changes to the wrong game.
    if (m_remapWindow.IsOpen()) return;

    if (!SelectProfile(MatchProfile(id))) return;
    // Only push a profile we are actually going to run. Applying one changes
    // the pad in place, but a profile whose platform differs rebuilds the
    // virtual controller outright — so pushing the default on the way out of
    // a game replugged the pad under the still-running game within a frame,
    // five seconds before the grace period below had any say in it. Leaving
    // whatever is live alone until the release really happens is what makes
    // alt-tabbing back genuinely free.
    if (WantControlNow()) PushActiveProfile();
    EvaluateControl();
}

void TrayApp::RefreshActiveProfile() {
    if (m_remapWindow.IsOpen()) return;
    SelectProfile(MatchProfile(ForegroundWatcher::Current()));
    if (WantControlNow()) PushActiveProfile();  // see OnForegroundChanged
    EvaluateControl();
}

bool TrayApp::SteamAllowsControl(SteamState state) const {
    switch (m_autoMode) {
    case AutoMode::OffWhileSteam: return state == SteamState::NoSteam;
    case AutoMode::OffOnlyInGame: return state != SteamState::InGame;
    default:                      return false;  // Manual: tray toggle decides
    }
}

// One question per mode, rather than one answer assembled from several
// settings that override each other — which is what makes the outcome
// predictable from the mode name alone.
bool TrayApp::WantControlNow() const {
    switch (m_autoMode) {
    case AutoMode::Manual:
        // The tray toggle is the only authority here, and nothing else may
        // turn the controller on behind the user's back.
        return m_wantControl;

    case AutoMode::OffUnlessProfile:
        // A matched profile is the entire test. Deliberately asked without
        // consulting Steam: the user made a profile for this game, which is
        // them saying they want us driving it rather than Steam Input, and
        // the acquire path already knows how to take the device from a live
        // Steam. With nothing matched we want nothing, so this mode never
        // contends with Steam except over games it was told to.
        return !m_activeGameId.empty();

    default:
        // The Steam-yielding modes, unchanged: Steam's state is the whole
        // answer, exactly as it has always been.
        return SteamAllowsControl(m_steamWatcher.GetState());
    }
}

void TrayApp::EvaluateControl() {
    const bool want = WantControlNow();
    EventLog::Write("CONTROL: want=%d (mode=%d steam=%d profile=%ls)",
                    want ? 1 : 0, static_cast<int>(m_autoMode),
                    static_cast<int>(m_steamWatcher.GetState()),
                    m_activeGameId.empty() ? L"none" : m_activeGameId.c_str());

    if (want) {
        // Alt-tabbed back before the grace period ran out — the release that
        // was pending is simply cancelled, and the pad never went anywhere.
        KillTimer(m_hwnd, IDT_RELEASE_GRACE);
        // Already ours, or an acquisition is still in flight. Setting
        // m_wantControl before TryAcquireController below is also what stops
        // the RefreshActiveProfile call on its success path re-entering here.
        if (m_wantControl) return;
        m_wantControl    = true;
        m_acquireRetries = 0;
        TryAcquireController();
        return;
    }

    if (!m_wantControl) return;

    // Steam turning up is the one release that cannot wait: the whole point
    // of yielding is that Steam is about to open the device, and sitting on
    // it for another few seconds is what it must not do.
    //
    // Only the Steam-yielding modes can be releasing for that reason.
    // OffUnlessProfile releases because the game stopped being in front,
    // which is precisely the case the grace period exists for — testing
    // SteamAllowsControl there would find the `false` it returns for every
    // mode it does not describe and make every release immediate.
    const bool yieldingToSteam = (m_autoMode == AutoMode::OffWhileSteam
                               || m_autoMode == AutoMode::OffOnlyInGame)
                              && !SteamAllowsControl(m_steamWatcher.GetState());
    if (yieldingToSteam) {
        ReleaseControl();
        return;
    }
    SetTimer(m_hwnd, IDT_RELEASE_GRACE, RELEASE_GRACE_MS, nullptr);
}

void TrayApp::ApplySteamState(SteamState state) {
    if (m_autoMode == AutoMode::Manual) return;

    // The state only. What to do about it is EvaluateControl's to decide and
    // to log, on the very next line — announcing a verdict here as well is
    // how this line came to report a yield in a mode that never consults
    // Steam and had in fact just taken the controller.
    EventLog::Write("AUTO: steam state %d (0=none 1=idle 2=inGame)",
                    static_cast<int>(state));
    EvaluateControl();
}

// Hand the controller back. Shared by the auto modes yielding to Steam and by
// the manual toggle being switched off — in both cases the user is done with
// us and Steam should be able to pick the pad straight back up.
void TrayApp::ReleaseControl() {
    KillTimer(m_hwnd, IDT_ACQUIRE);
    KillTimer(m_hwnd, IDT_ACQUIRE_VERDICT);
    KillTimer(m_hwnd, IDT_WAKE_POLL);
    KillTimer(m_hwnd, IDT_RELEASE_GRACE);
    m_wantControl    = false;
    m_acquireRetries = 0;
    m_lastCycleTick  = 0;
    m_cycleInFlight  = false;  // cleared with the tick it is measured against
    // One driver notice per attempt, and letting go ends the attempt. Turning
    // Steamless Mode back on is a fresh ask and deserves a fresh answer.
    m_vigemBalloonShown = false;
    // Deliberately leaves the selected profile alone. It used to be dropped
    // here, on the grounds that a profile belongs to a controller we no
    // longer have — but the selection is now what decides whether we take the
    // controller in the first place, so clearing it here would erase the
    // reason we are releasing and call straight back into this function. The
    // foreground watcher keeps it current whether or not the device is ours,
    // which is what that reset was standing in for.
    const bool hadControl = m_controller->IsGameModeActive();
    // Good citizen: restore lizard mode and close our HID handles so
    // Steam can claim the controller without contention.
    m_controller->ReleaseDevices();
    // Steam only (re)opens controllers on device-arrival events. If it is
    // already running and we held the device, it never saw one — cycle
    // the device so Steam adopts it immediately.
    if (hadControl && m_steamWatcher.GetState() != SteamState::NoSteam) {
        // Logged because this cycle is otherwise invisible. The acquire path
        // announces itself; this one used to not, which made a device left
        // disabled by an interrupted release look like it came from nowhere.
        EventLog::Write("RELEASE: cycling device so Steam sees an arrival");
        RestartControllerDevices();
    }
}

void TrayApp::TryAcquireController(uint32_t stateWaitMs) {
    // Do not open the device while a cycle we asked for is still running. The
    // claim below opens handles, the cycle needs every handle gone to take the
    // devnode down, and PnP does not care that the handle blocking it is ours —
    // it vetoes the cycle just the same. Reported from the field as the app
    // cycling the puck against a handle held by SteamlessController.exe itself,
    // which left the vendor collections disabled.
    //
    // The arrival that ends a cycle clears this, so the reopen race against
    // Steam is not slowed down; the tick is only a backstop for a cycle that
    // never produces one.
    if (m_cycleInFlight) {
        if (GetTickCount64() - m_lastCycleTick < CYCLE_MIN_GAP_MS) {
            // Once per cycle: a receiver publishes an interface per slot and
            // every arrival re-enters here, so saying it each time would bury
            // the log in repeats of a single fact.
            if (!m_inFlightLogged) {
                m_inFlightLogged = true;
                EventLog::Write("ACQUIRE: cycle still in flight — not opening the device "
                                "(our own handle would veto it)");
            }
            SetTimer(m_hwnd, IDT_ACQUIRE, ACQUIRE_RETRY_MS, nullptr);
            return;
        }
        EventLog::Write("ACQUIRE: cycle produced no arrival within %llu ms — claiming anyway",
                        CYCLE_MIN_GAP_MS);
        m_cycleInFlight = false;
    }

    m_controller->OnDeviceChange();
    auto outcome = m_controller->EnableGameMode(stateWaitMs);

    // A short burst of rapid retries: right after a device cycle we race
    // Steam's re-enumeration for the exclusive open, and starting the claim
    // the instant the device arrives is what wins that race.
    //
    // A missing virtual pad is not a race with anybody, so the burst is called
    // off the moment that is the answer. Left running it re-attempted eleven
    // times in half a second and re-armed the driver notification on each
    // pass — the toast storm reported in #68.
    for (int i = 0; i < 10
                 && outcome != ControllerManager::GameModeOutcome::VirtualPadUnavailable
                 && !m_controller->IsGameModeActive(); ++i) {
        Sleep(50);
        m_controller->OnDeviceChange();
        outcome = m_controller->EnableGameMode(stateWaitMs);
    }
    if (m_controller->IsGameModeActive()) {
        KillTimer(m_hwnd, IDT_ACQUIRE_VERDICT);
        KillTimer(m_hwnd, IDT_WAKE_POLL);
        m_acquireRetries = 0;
        m_lastCycleTick  = 0;
        // The controller is ours again, and the user may have been sitting in
        // a game the whole time it was not — no foreground change is coming
        // to tell us, so ask.
        RefreshActiveProfile();
        return;
    }
    if (!m_controller->IsConnected()) return;  // nothing plugged in — arrival will retrigger

    // The controller is fine; we just have nowhere to send its input. None of
    // the machinery below applies — the retry counter, the cycling, and the
    // holder scan all exist to get a HID handle away from another process,
    // and no amount of restarting the puck installs a bus driver. Cycling
    // here is exactly what disabled a reporter's HID collections in #65.
    //
    // So sit on a slow timer instead. The user is being told what to install,
    // and this is what notices once they have.
    if (outcome == ControllerManager::GameModeOutcome::VirtualPadUnavailable) {
        // A verdict left pending by an earlier contested attempt would report
        // this as Steam holding the controller, which it plainly is not.
        KillTimer(m_hwnd, IDT_ACQUIRE_VERDICT);
        SetTimer(m_hwnd, IDT_ACQUIRE, VIGEM_RETRY_MS, nullptr);
        return;
    }

    // No slot is emitting state: the controller is off, asleep, or the
    // receiver has no controller paired into it. Cycling cannot conjure one
    // up, and doing it anyway restarts the receiver's devnodes (and can
    // prompt for elevation) just because nothing is switched on.
    //
    // Poll instead of waiting for an event. A receiver keeps publishing all
    // of its slot interfaces whether or not a controller is paired into one,
    // so switching the controller on produces no WM_DEVICECHANGE at all —
    // measured: 88 seconds of silence after the controller was turned on.
    if (outcome == ControllerManager::GameModeOutcome::NoActiveController) {
        SetTimer(m_hwnd, IDT_WAKE_POLL, WAKE_POLL_MS, nullptr);
        return;
    }

    // A cycle is asynchronous — fired through Task Scheduler, and the helper
    // pauses a second between disable and enable. Every device arrival it
    // produces re-enters this function, so without a floor on the spacing we
    // stack another cycle on top of one still in flight. Observed: game mode
    // came up at :55.355 and our own second cycle ripped the device back out
    // at :56.367. Wait for the one already running to finish instead.
    const ULONGLONG nowTick = GetTickCount64();
    if (m_lastCycleTick != 0 && nowTick - m_lastCycleTick < CYCLE_MIN_GAP_MS) {
        SetTimer(m_hwnd, IDT_ACQUIRE, ACQUIRE_RETRY_MS, nullptr);
        return;
    }

    // The last cycle was blocked by a handle of our own. Another cycle would be
    // blocked by the same thing, and each attempt risks leaving the devnode
    // switched off, so let go instead of restarting hardware to escape
    // ourselves. Releasing costs nothing if it turns out we held nothing.
    if (m_acquireRetries > 0 && RefreshCycleStatus() && m_lastCycleStatus.selfHeld) {
        EventLog::Write("ACQUIRE: the last cycle was vetoed by this app's own handle — "
                        "releasing it instead of cycling again");
        m_controller->ReleaseDevices();
        SetTimer(m_hwnd, IDT_ACQUIRE, ACQUIRE_RETRY_MS, nullptr);
        return;
    }

    // A cycle that could not take the devnode down proves the next one will do
    // exactly as much: nothing. Stop rather than burning the remaining attempts
    // on a restart that is known to leave every handle alive — that cost 26
    // seconds and three pointless device restarts before it was measured.
    if (m_acquireRetries > 0 && LastCycleBrokeNothing()) {
        EventLog::Write("ACQUIRE: last cycle invalidated no handles (%s) — not cycling again",
                        DeviceRestart::CycleKindName(m_lastCycleStatus.kind));
        // Still let the verdict timer deliver the news: the last cycle's
        // re-arrival can be in flight and may yet hand us the controller.
        SetTimer(m_hwnd, IDT_ACQUIRE_VERDICT, ACQUIRE_VERDICT_MS, nullptr);
        return;
    }

    // Steam holds a write handle, so our exclusive claim can't succeed while
    // its handle lives. Cycle the device to invalidate it and retry on
    // re-arrival (the timer is a fallback in case the arrival event is missed).
    if (m_acquireRetries >= MAX_ACQUIRE_CYCLES) {
        // Don't announce failure yet. The last cycle's re-arrival can still be
        // in flight — on a four-interface receiver the round trip outran the
        // retry timer, so the app cried failure a second before succeeding.
        // Let the grace timer deliver the verdict instead.
        SetTimer(m_hwnd, IDT_ACQUIRE_VERDICT, ACQUIRE_VERDICT_MS, nullptr);
        return;
    }
    ++m_acquireRetries;
    m_lastCycleTick = nowTick;
    EventLog::Write("ACQUIRE: exclusive claim blocked — cycling device (attempt %d)", m_acquireRetries);
    m_controller->ReleaseDevices();
    if (!RestartControllerDevices()) return;  // helper unavailable — balloon shown
    // The cycle runs asynchronously via the helper task; the device-arrival
    // notification drives the claim, with this timer as the fallback. It must
    // outlast a cycle: the helper waits a second between disable and enable,
    // then a multi-slot receiver has to re-enumerate every interface.
    SetTimer(m_hwnd, IDT_ACQUIRE, ACQUIRE_RETRY_MS, nullptr);
}

// Pick up the verdict of the cycle we last started. It runs in the elevated
// helper, so the answer comes back through a file — and anything stamped
// before our cycle began belongs to an earlier attempt and says nothing about
// this one.
bool TrayApp::RefreshCycleStatus() {
    if (m_lastCycleTick == 0) return false;
    DeviceRestart::CycleResult status;
    if (!DeviceRestart::ReadCycleStatus(status)) return false;
    // Ticks are milliseconds since boot, so a file left by a previous boot is
    // not comparable to ours. One stamped in the future has to be from a
    // longer-running earlier session — the ordinary case after a reboot, and
    // one that would otherwise pass the staleness test below.
    if (status.ticks > GetTickCount64()) return false;
    if (status.ticks < m_lastCycleTick) return false;
    m_lastCycleStatus = status;
    return true;
}

bool TrayApp::LastCycleBrokeNothing() {
    return RefreshCycleStatus() && m_lastCycleStatus.BrokeNoHandles();
}

// Explain the failure in terms of what actually happened. The app used to
// blame Steam unconditionally, which was wrong whenever some other process
// was sitting on the device — the case that is impossible to diagnose from
// the outside and the most likely one to leave a user stuck.
void TrayApp::ReportAcquireFailure() {
    RefreshCycleStatus();
    const DeviceRestart::CycleResult& st = m_lastCycleStatus;

    // Everything the scan learned goes in this log, not just the conclusion:
    // the unfiltered holder list and how much of the system the scan actually
    // covered. This is the log "Open Event Log" opens, so it is the one that
    // reaches a maintainer when a user reports a controller they cannot take
    // back — and by then the process holding it is long gone.
    if (!st.scanInfo.empty())
        EventLog::Write("ACQUIRE: holder scan %ls", st.scanInfo.c_str());
    if (!st.holdersAll.empty())
        EventLog::Write("ACQUIRE: processes holding the device open: %ls "
                        "(steam.exe and SteamlessController.exe are expected here)",
                        st.holdersAll.c_str());
    else if (!st.scanInfo.empty())
        EventLog::Write("ACQUIRE: no process holding the device could be identified "
                        "— a protected process, or the scan did not reach it");

    if (st.HeldByAnotherProcess()) {
        EventLog::Write("ACQUIRE: giving up — another process holds the device "
                        "(veto=%s holders=%ls)",
                        DeviceRestart::VetoTypeName(st.vetoType),
                        st.holders.empty() ? L"(none identified)" : st.holders.c_str());
        if (!m_notificationsEnabled) return;
        std::wstring text;
        // A replug is worth offering: Windows will not take the device down
        // while a handle is held, at this node or any parent, but pulling the
        // cable is a surprise removal that no software veto can stop.
        if (!st.holders.empty())
            text = st.holders
                 + L" has the controller open, and Windows will not restart the "
                   L"device while it is held. Close it, or unplug and replug the "
                   L"controller.";
        else if (st.VetoNamesTheHolder())
            text = L"\"" + st.vetoName
                 + L"\" has the controller open, and Windows will not restart the "
                   L"device while it is held. Close it, or unplug and replug the "
                   L"controller.";
        else
            text = L"Another program has the controller open, and Windows will not "
                   L"restart the device while it is held. Close any other controller "
                   L"tools, or unplug and replug the controller.";
        ShowAlertBalloon(L"Could not take the controller", text);
        return;
    }

    if (st.ticks != 0 && st.BrokeNoHandles()) {
        EventLog::Write("ACQUIRE: giving up — the cycle never took the device down (%s err=%lu)",
                        DeviceRestart::CycleKindName(st.kind), st.error);
        if (m_notificationsEnabled)
            ShowAlertBalloon(L"Could not take the controller",
                             L"Windows would not restart the controller device, so it "
                             L"could not be taken from Steam. Unplugging and replugging "
                             L"it will hand it over.");
        return;
    }

    EventLog::Write("ACQUIRE: giving up after %d device cycles", MAX_ACQUIRE_CYCLES);
    if (m_notificationsEnabled)
        ShowAlertBalloon(L"Could not take the controller",
                         L"Steam is holding the controller and did not release it. "
                         L"Closing Steam will hand it back.");
}

bool TrayApp::RunPendingRecovery() {
    if (IsProcessElevated()) {
        std::wstring report;
        const int recovered = DeviceRestart::ReconcilePendingDisables(&report);
        EventLog::Write("RECOVER: %ls (%d devnode(s) re-enabled)",
                        report.empty() ? L"nothing was still disabled" : report.c_str(),
                        recovered);
        return true;
    }

    // Enabling a devnode needs elevation, so this has to go through the helper.
    // Its scheduled task takes no arguments — schtasks cannot pass any — so it
    // runs the ordinary entry point, which reconciles first and then cycles.
    // Both are wanted here: the device comes back, and it comes back presented
    // as a fresh arrival that Steam and we can race for as usual.
    //
    // Asynchronous: schtasks returns once the task has been started, not once
    // the helper has finished, so the device reappears on its own a moment later
    // as a normal arrival rather than before this returns.
    return RestartControllerDevices();
}

// A silent log is ambiguous. Nothing is written while the app sits idle with
// nothing changing — AUTO lines only fire on Steam state *changes* — and
// nothing is written while it is wedged or dead either. So a gap means either
// "healthy, with nothing to say" or "stopped working", and after the fact there
// is no way to tell which. That ambiguity cost a real investigation: 33 hours
// of silence that read as a hang and was very likely an idle app.
//
// Driven by the window timer deliberately. A background thread would only prove
// the process still exists; this proves the message loop is still being pumped,
// which is what "not hung" means here — the acquire and release paths both run
// on it, and both can sit inside SetupAPI for seconds at a time.
void TrayApp::WriteHeartbeat() {
    ULONGLONG unbiased = 0;
    QueryUnbiasedInterruptTime(&unbiased);
    const ULONGLONG sinceMs = m_lastHeartbeatUnbiased
        ? (unbiased - m_lastHeartbeatUnbiased) / 10000ULL
        : 0;
    m_lastHeartbeatUnbiased = unbiased;

    // Unbiased time does not advance while the machine is suspended, so a beat
    // that is late by this measure was genuinely blocked rather than closed in
    // a laptop lid. Worth its own line: a stall that recovers leaves no other
    // trace, and the beat before a gap is the last sign of life anyone gets.
    if (sinceMs > HEARTBEAT_MS + HEARTBEAT_SLACK_MS)
        EventLog::Write("HEARTBEAT: the message loop was blocked for %llu s "
                        "(beat due every %u s)",
                        sinceMs / 1000, HEARTBEAT_MS / 1000);

    const ULONGLONG upMs = GetTickCount64() - m_startTick;
    EventLog::Write("HEARTBEAT: alive uptime=%lluh%02llum controller=%s gameMode=%s "
                    "steam=%d (0=none 1=idle 2=inGame) mode=%d wantControl=%d",
                    upMs / 3600000ULL, (upMs / 60000ULL) % 60ULL,
                    m_controller->IsConnected() ? "connected" : "absent",
                    m_controller->IsGameModeActive() ? "on" : "off",
                    static_cast<int>(m_steamWatcher.GetState()),
                    static_cast<int>(m_autoMode),
                    m_wantControl ? 1 : 0);
}

// Undo a disable an earlier run committed to the registry but never reversed.
// Cheap and silent in the normal case: with no pending record this is one
// failed file open.
void TrayApp::RecoverStrandedDevices() {
    if (!DeviceRestart::HasPendingDisable()) {
        // No record of our own, but the device can still be switched off — by
        // Device Manager, by regedit, by pnputil, by a tool that hides HID
        // devices. Not something to undo silently: it may well be deliberate.
        // Naming it is the useful part, because from here it is otherwise
        // indistinguishable from no controller being plugged in at all.
        m_disabledDeviceNode = DeviceRestart::FindDisabledControllerNode();
        if (!m_disabledDeviceNode.empty()) {
            EventLog::Write("STARTUP: a controller devnode is disabled in Windows, so it "
                            "publishes no interfaces and cannot be seen here or by Steam: "
                            "%ls", m_disabledDeviceNode.c_str());
            EventLog::Write("STARTUP: re-enable it from the tray menu, in Device Manager, "
                            "or with: pnputil /enable-device \"%ls\"",
                            m_disabledDeviceNode.c_str());
            if (m_notificationsEnabled)
                ShowAlertBalloon(L"Controller is disabled in Windows",
                                 L"Windows has this controller's device switched off, so "
                                 L"nothing can see it — including Steam. Click here to "
                                 L"re-enable it.",
                                 BalloonAction::EnableDisabledDevice);
        }
        return;
    }

    EventLog::Write("RECOVER: a previous run was interrupted mid-cycle and left the "
                    "controller's devnode disabled — putting it back");

    if (!RunPendingRecovery())
        EventLog::Write("RECOVER: the elevated helper could not be run — the controller "
                        "stays disabled until it is re-enabled in Device Manager "
                        "(or: pnputil /enable-device \"<instance id>\")");
}

void TrayApp::EnableDisabledControllerDevice() {
    // Re-resolve rather than trusting what the menu or balloon was built from:
    // minutes may have passed, and the user may already have fixed it by hand.
    const std::wstring node = DeviceRestart::FindDisabledControllerNode();
    m_disabledDeviceNode = node;
    if (node.empty()) {
        EventLog::Write("USER: asked to re-enable the controller device, but no controller "
                        "devnode is disabled");
        return;
    }

    EventLog::Write("USER: re-enabling disabled controller devnode %ls", node.c_str());

    // Hand it to the same mechanism that repairs an interrupted cycle rather
    // than growing a second way to switch a devnode on: recorded as pending, it
    // is undone by exactly the path that has already been exercised. The parent
    // is left blank — this node is known to be the disabled one, so there is
    // nothing to guess at.
    DeviceRestart::ArmPendingDisable(node, L"");

    if (!RunPendingRecovery()) {
        EventLog::Write("USER: could not run the elevated helper to re-enable the device");
        if (m_notificationsEnabled)
            ShowAlertBalloon(L"Could not re-enable the controller",
                             L"Windows would not let the device be switched back on from "
                             L"here. Re-enable it in Device Manager, under Human Interface "
                             L"Devices.");
        return;
    }
    m_disabledDeviceNode.clear();
}

bool TrayApp::RestartControllerDevices() {
    // Every cycle starts here, so the "do not reopen while this runs" flag is
    // raised here rather than at each call site — the recovery cycle can be
    // vetoed by our own handle exactly as readily as the acquire one, and the
    // tick it is measured against has to move with it.
    m_cycleInFlight  = true;
    m_inFlightLogged = false;
    m_lastCycleTick  = GetTickCount64();

    // On the rare elevated run, cycle directly. This works on every transport
    // including Bluetooth: the devnode being restarted is the HID child, not
    // the pairing (which lives up at the BTHLEDEVICE layer), and it
    // re-enumerates as fast as a USB replug.
    if (IsProcessElevated()) {
        bool anyRestarted = false;
        DeviceRestart::CycleResult summary;
        for (const auto& path : SteamController::EnumerateAll()) {
            DeviceRestart::CycleResult r;
            if (DeviceRestart::RestartInterfaceDevice(path, r))
                anyRestarted = true;
            const bool better = r.kind > summary.kind
                || (r.kind == summary.kind
                    && summary.vetoType == PNP_VetoTypeUnknown
                    && r.vetoType != PNP_VetoTypeUnknown);
            if (better) summary = r;
        }
        // Same status file the helper writes, so the verdict logic below does
        // not care which path performed the cycle.
        summary.ticks = GetTickCount64();
        DeviceRestart::WriteCycleStatus(summary);
        // Synchronous: the cycle has already finished and the device is back,
        // so there is nothing left to hold the claim off for.
        m_cycleInFlight = false;
        return anyRestarted;
    }

    // Normal path: fire the elevated helper via its scheduled task. Starting
    // a task the user authored needs no elevation, so no UAC prompt here.
    std::wstring run = L"schtasks.exe /Run /TN \"";
    run += CYCLE_TASK_NAME;
    run += L"\"";
    if (RunToolHidden(run)) return true;  // asynchronous — flag stays raised

    // Task missing (copied exes without installing?) — register it with a
    // one-time UAC prompt, then retry.
    if (EnsureCycleTaskRegistered() && RunToolHidden(std::move(run)))
        return true;

    // No cycle was started, so nothing is running that our handle could veto.
    m_cycleInFlight = false;
    return false;
}

void TrayApp::ShowElevationBalloon() {
    if (m_elevationBalloonShown) return;
    m_elevationBalloonShown = true;

    NOTIFYICONDATAW nid{};
    nid.cbSize      = sizeof(nid);
    nid.hWnd        = m_hwnd;
    nid.uID         = TRAY_UID;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
    wcscpy_s(nid.szInfoTitle, L"One-time setup needed");
    wcscpy_s(nid.szInfo,
             L"Approve the administrator prompt to enable in-game controller "
             L"handoff (select the mode again to retry).");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayApp::OpenRemapWindow() {
    // The moment a user is most likely to have installed something since the
    // app started, and the moment a stale list would matter most — profiles
    // created here are matched against it.
    RefreshSteamApps();

    // Synchronous on the UI thread — the Start Menu walk plus the
    // shell:AppsFolder enumeration together are still fast enough in
    // practice (local disk and COM calls, no network) to not need a
    // background thread for an action the user just explicitly asked to
    // open a window for.
    m_remapWindow.Open(
        m_hInstance,
        m_controller.get(),
        m_defaultProfile,
        GameLibrary::EnumerateInstalled(),
        m_gameProfiles,
        [this](const std::wstring& gameId, const ControllerProfile& profile) {
            if (gameId.empty()) {
                m_defaultProfile = profile;
                SaveSettings();
            } else {
                m_gameProfiles[gameId] = profile;
                GameProfiles::Save(m_gameProfiles);
            }
            // Editing whichever profile is live should be visible straight
            // away. Asked unconditionally rather than by comparing ids,
            // because the live profile may not be the edited one but may be
            // following it — only ActiveProfile knows that, and for anything
            // genuinely unrelated this resolves to what is already loaded.
            PushActiveProfile();
        },
        [this](const std::wstring& gameId) {
            m_gameProfiles.erase(gameId);
            GameProfiles::Save(m_gameProfiles);
            EventLog::Write("PROFILE: deleted %ls", gameId.c_str());
            // No control decision here, for the same reason applying makes
            // none: the window is open and may have a row listening for a
            // button, and dropping the controller under it would end that
            // mid-capture. ActiveProfile already resolves a now-missing id to
            // the default, and closing the window re-evaluates — which is
            // what lets go of a controller this profile was holding.
            PushActiveProfile();
        });
}

void TrayApp::AddTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = m_hwnd;
    nid.uID              = TRAY_UID;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon            = m_iconOff;
    wcscpy_s(nid.szTip, L"Steamless Controller");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void TrayApp::RemoveTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_hwnd;
    nid.uID    = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void TrayApp::UpdateTrayIcon(bool connected, bool gameModeActive, bool sharedHandle,
                             bool padUnavailable) {
    if (padUnavailable) ShowViGEmBalloon();
    // Cleared only by a pad that actually came up, not by any notification
    // that happens to carry false. The acquire path notifies repeatedly within
    // a single attempt, so clearing on false re-armed the balloon between
    // consecutive failures and turned one attempt into a burst of toasts.
    // ReleaseControl clears it too, so a deliberate re-enable says it again.
    else if (gameModeActive) m_vigemBalloonShown = false;

    // Sharing is a distinct running state, not a degraded one: game mode works,
    // but another process is on the same controller and can fight us for it.
    // Worth its own icon so a user chasing odd input knows to look at Steam.
    const bool shared = gameModeActive && sharedHandle;

    // Fall through rather than returning early — the icon and tooltip still need
    // to track the controller while the driver is unavailable.
    const wchar_t* tip = shared          ? L"Steamless Controller — Steamless Mode ON (sharing with Steam)"
                       : gameModeActive  ? L"Steamless Controller — Steamless Mode ON"
                       : padUnavailable  ? L"Steamless Controller — no virtual gamepad bus"
                       : connected       ? L"Steamless Controller — Connected (Steamless Mode OFF)"
                                         : L"Steamless Controller — No controller found";

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_hwnd;
    nid.uID    = TRAY_UID;
    nid.uFlags = NIF_TIP | NIF_ICON;
    nid.hIcon  = shared         ? m_iconShared
               : gameModeActive ? m_iconOn
                                : m_iconOff;
    wcscpy_s(nid.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayApp::ShowAlertBalloon(const std::wstring& title, const std::wstring& text,
                              BalloonAction action, DWORD infoFlags) {
    m_balloonAction = action;

    // Every balloon the app raises, recorded where it is raised. Notifications
    // are the one thing this app does that leaves no other trace, so a report
    // of seeing one twice was otherwise unanswerable — the log could show a
    // profile loading once and say nothing about how many times Windows drew
    // the toast. Two lines here means we asked twice; one means Windows drew
    // it twice, and those want completely different fixes.
    EventLog::Write("BALLOON: %ls — %ls", title.c_str(), text.c_str());

    NOTIFYICONDATAW nid{};
    nid.cbSize      = sizeof(nid);
    nid.hWnd        = m_hwnd;
    nid.uID         = TRAY_UID;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = infoFlags;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo,      text.c_str(),  _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayApp::OpenEventLog() {
    // Touch the log so the file exists even on a fresh install.
    EventLog::Write("USER: opened event log");
    // Open with Notepad explicitly — .log has no guaranteed file association.
    ShellExecuteW(nullptr, L"open", L"notepad.exe",
                  EventLog::FilePath().c_str(), nullptr, SW_SHOWNORMAL);
}

// Latched: every failed game-mode enable calls back into UpdateTrayIcon, which
// during an acquire-retry loop is several times a second — without this the user
// gets a wall of identical balloons. UpdateTrayIcon clears the latch once the
// driver is reachable again, so a genuine mid-session disappearance still warns.
void TrayApp::ShowViGEmBalloon() {
    if (!m_notificationsEnabled) return;
    if (m_vigemBalloonShown) return;
    m_vigemBalloonShown = true;

    // Two different problems wear the same failure internally, and the advice
    // for one is wrong for the other. Telling somebody who already has a bus
    // to go install ViGEmBus is what #65 spent three weeks on, so say which
    // case this is — the manager established it by enumerating, not by
    // guessing from an error code.
    //
    // Missing is by far the likelier of the two: the one confirmed way to get
    // here is an installer that skipped ViGEmBus (#68). The other branch names
    // no culprit on purpose — a bus that is present and unusable has several
    // possible causes and we have never seen one in the field.
    const bool missing = m_controller->LastPadDriverMissing();
    m_balloonAction = missing ? BalloonAction::ViGEmDownload : BalloonAction::None;

    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = m_hwnd;
    nid.uID              = TRAY_UID;
    nid.uFlags           = NIF_INFO;
    nid.dwInfoFlags      = NIIF_WARNING;
    wcscpy_s(nid.szInfoTitle, missing ? L"Driver required" : L"Virtual gamepad unavailable");
    wcscpy_s(nid.szInfo,
             missing
                 ? L"ViGEmBus is unavailable — not installed, or disabled in Device "
                   L"Manager under System devices. Click here to download it."
                 : L"A virtual gamepad bus is present but would not accept a "
                   L"controller. It may be disabled, or a version this app cannot "
                   L"talk to. The event log has the details.");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static constexpr wchar_t REG_KEY[]     = L"Software\\SteamlessController";
static constexpr wchar_t REG_RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr wchar_t APP_NAME[]    = L"SteamlessController";

bool TrayApp::RunKeyExists() const {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    bool exists = RegQueryValueExW(key, APP_NAME, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return exists;
}

void TrayApp::SetRunKey(bool enabled) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_WRITE, &key) != ERROR_SUCCESS)
        return;

    if (enabled) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        RegSetValueExW(key, APP_NAME, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(path),
                       static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, APP_NAME);
    }

    RegCloseKey(key);
}

bool TrayApp::IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elev{};
    DWORD size = sizeof(elev);
    const bool ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size) != 0;
    CloseHandle(token);
    return ok && elev.TokenIsElevated != 0;
}

// The executable the cycle task is currently registered to run, or empty when
// the task is missing or unreadable.
static std::string RegisteredTaskCommand() {
    wchar_t tempDir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tempDir)) return {};
    const std::wstring xmlPath = std::wstring(tempDir) + L"SteamlessTaskQuery.xml";

    // Via cmd.exe for the redirect. The XML declares encoding="UTF-16", but
    // redirected output is single-byte — read it narrow, don't "fix" this.
    std::wstring cmd = L"cmd.exe /c schtasks.exe /Query /TN \"";
    cmd += CYCLE_TASK_NAME;
    cmd += L"\" /XML > \"" + xmlPath + L"\"";
    const bool queried = RunToolHidden(std::move(cmd));

    std::string xml;
    if (queried) {
        if (FILE* f = nullptr; _wfopen_s(&f, xmlPath.c_str(), L"rb") == 0 && f) {
            char buf[1024];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                xml.append(buf, n);
            fclose(f);
        }
    }
    DeleteFileW(xmlPath.c_str());
    if (xml.empty()) return {};

    const size_t open = xml.find("<Command>");
    if (open == std::string::npos) return {};
    const size_t start = open + strlen("<Command>");
    const size_t close = xml.find("</Command>", start);
    if (close == std::string::npos) return {};

    std::string cmdPath = xml.substr(start, close - start);
    const size_t first = cmdPath.find_first_not_of(" \t\r\n");
    const size_t last  = cmdPath.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return cmdPath.substr(first, last - first + 1);
}

// True when the registered command is this build's helper. Compared against
// both codepage renderings of our path, since which one the console redirect
// used is not worth guessing at.
static bool RegisteredCommandMatches(const std::string& registered,
                                     const std::wstring& helper) {
    if (registered.empty()) return false;
    for (UINT cp : { CP_ACP, CP_OEMCP }) {
        const int n = WideCharToMultiByte(cp, 0, helper.c_str(), -1,
                                          nullptr, 0, nullptr, nullptr);
        if (n <= 0) continue;
        std::string narrow(static_cast<size_t>(n) - 1, '\0');
        WideCharToMultiByte(cp, 0, helper.c_str(), -1, narrow.data(), n, nullptr, nullptr);
        if (_stricmp(narrow.c_str(), registered.c_str()) == 0) return true;
    }
    return false;
}

bool TrayApp::EnsureCycleTaskRegistered() {
    // Registered AND pointing at this build's helper? Existence alone is not
    // enough: a task left by an older or relocated install still "runs" —
    // schtasks reports success — while executing a helper that may be gone or
    // predate current device support, so the cycle silently does nothing and
    // the controller is never reclaimed. That failure is invisible from the
    // app's side, so verify the path rather than just the task.
    {
        const std::string registered = RegisteredTaskCommand();
        if (RegisteredCommandMatches(registered, HelperPath()))
            return true;
        if (!registered.empty())
            EventLog::Write("CYCLE TASK: registered helper is stale, re-registering");
    }

    // One-time UAC prompt: the helper registers its own on-demand task.
    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    const std::wstring helper = HelperPath();
    sei.lpVerb       = L"runas";
    sei.lpFile       = helper.c_str();
    sei.lpParameters = L"--register";
    sei.nShow        = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        // UAC declined — in-game mode degrades: we can't cycle the device
        // away from a running Steam, but takeover when Steam exits still works.
        ShowElevationBalloon();
        return false;
    }
    WaitForSingleObject(sei.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code != 0) ShowElevationBalloon();
    return code == 0;
}

void TrayApp::DeleteLegacyStartupTask() {
    // A pre-release build registered an elevated logon task for the tray app
    // itself; running elevated at logon resurrects the UIPI cursor-freeze
    // bug, so remove it best-effort (also done by the helper's --register).
    std::wstring cmd = L"schtasks.exe /Delete /F /TN \"";
    cmd += LEGACY_STARTUP_TASK_NAME;
    cmd += L"\"";
    RunToolHidden(std::move(cmd));
}

void TrayApp::UpdateStartupRegistration() {
    // Startup always uses the HKCU Run key — the tray app never runs
    // elevated, so the Run key works unconditionally.
    if (m_startupMechanism == 2) DeleteLegacyStartupTask();
    const int desired = m_startupEnabled ? 1 : 0;
    SetRunKey(desired == 1);
    m_startupMechanism = desired;
    SaveSettings();
}

void TrayApp::LoadSettings() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;

    auto readDw = [&](const wchar_t* name, DWORD def) -> DWORD {
        DWORD val = 0, size = sizeof(val);
        if (RegQueryValueExW(key, name, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS)
            return val;
        return def;
    };

    // 0 = Manual, 1 = OffWhileSteam, 2 = OffOnlyInGame, 3 = OffUnlessProfile
    // (old builds stored 0/1). Anything unrecognised falls back to Manual,
    // which is the mode that touches nothing on its own.
    const DWORD mode = readDw(L"AutoSteamMode", 0);
    m_autoMode = mode == 3 ? AutoMode::OffUnlessProfile
               : mode == 2 ? AutoMode::OffOnlyInGame
               : mode == 1 ? AutoMode::OffWhileSteam
                           : AutoMode::Manual;
    const bool isPlayStation = readDw(L"ControllerPlatform", 0) != 0;

    // Existing installs keep exactly the bindings they had — the new
    // click-by-default only applies to fresh installs, which never reach here
    // and so take the BackButtonConfig defaults.
    const DWORD unbound = BackButtonBinding::FromAction(BackButtonAction::None).Pack();
    ControllerProfile profile;
    profile.platform = isPlayStation ? ControllerPlatform::PlayStation
                                     : ControllerPlatform::Xbox;
    profile.back.l4 = BackButtonBinding::Unpack(readDw(L"BackBtnL4", unbound));
    profile.back.l5 = BackButtonBinding::Unpack(readDw(L"BackBtnL5", unbound));
    profile.back.r4 = BackButtonBinding::Unpack(readDw(L"BackBtnR4", unbound));
    profile.back.r5 = BackButtonBinding::Unpack(readDw(L"BackBtnR5", unbound));

    // Migrate the retired "Back Buttons as Mouse Click" toggle into real
    // bindings. The toggle used to hijack L4/R4 for a left click and silently
    // discard whatever they were bound to, so only fill in the paddles that
    // have no binding of their own — an explicit mapping was the user's intent
    // all along and starts working again now that nothing overrides it.
    const bool migrateBackMouse = readDw(L"BackButtons", 0) != 0;
    if (migrateBackMouse) {
        const auto leftClick = BackButtonBinding::FromAction(BackButtonAction::LeftMouseButton);
        if (profile.back.l4.IsAction(BackButtonAction::None)) profile.back.l4 = leftClick;
        if (profile.back.r4.IsAction(BackButtonAction::None)) profile.back.r4 = leftClick;
    }

    // Per-pad settings. Installs that predate them carry the two retired
    // toggles instead — one pad, mouse or nothing — which is expressible in
    // the new shape exactly: the chosen pad becomes a pointer whose click is
    // the left button it was always hardwired to, and the other pad stays
    // unclaimed. "PadsMigrated" marks the conversion done, so a later choice
    // of "no pad at all" is not overwritten on the next launch by the stale
    // toggles this reads.
    const bool migratePads = readDw(L"PadsMigrated", 0) == 0;
    if (migratePads) {
        // A pad the old build was not using for the mouse still reached the
        // game as a DS4 touchpad on PlayStation, and did nothing at all on
        // Xbox. Both are now explicit modes, so the migration has to name the
        // right one rather than let them share a default.
        //
        // Assigned whole rather than field by field: ControllerProfile's
        // defaults describe a fresh install, and an upgrade is not one. Left
        // partially overwritten, the pad that was not driving the mouse would
        // pick up the fresh-install click binding it never had.
        const TrackpadMode idle = isPlayStation ? TrackpadMode::DS4Touchpad
                                                : TrackpadMode::None;
        profile.leftPad  = TrackpadSettings{ .mode = idle };
        profile.rightPad = TrackpadSettings{ .mode = idle };
        if (readDw(L"TrackpadMouse", 0) != 0) {
            auto& pad = readDw(L"UseLeftTrackpad", 0) != 0 ? profile.leftPad
                                                           : profile.rightPad;
            pad.mode  = TrackpadMode::MousePointer;
            pad.click = BackButtonBinding::FromAction(BackButtonAction::LeftMouseButton);
        }
    } else {
        profile.leftPad.mode       = TrackpadModeFromDword(readDw(L"LeftPadMode", 0));
        profile.leftPad.click      = BackButtonBinding::Unpack(readDw(L"LeftPadClick",  unbound));
        profile.leftPad.scrollDir  = ScrollDirectionFromDword(readDw(L"LeftPadScrollDir", 0));
        profile.rightPad.mode      = TrackpadModeFromDword(readDw(L"RightPadMode", 0));
        profile.rightPad.click     = BackButtonBinding::Unpack(readDw(L"RightPadClick", unbound));
        profile.rightPad.scrollDir = ScrollDirectionFromDword(readDw(L"RightPadScrollDir", 0));
    }

    m_defaultProfile = profile;
    m_activeGameId.clear();
    m_controller->SetProfile(m_defaultProfile);

    m_notificationsEnabled = readDw(L"ShowNotifications", 1) != 0;

    // Startup mechanism: 0 none, 1 Run key, 2 elevated task. Migrate installs
    // that predate the setting by probing the Run key they would have used.
    m_startupMechanism = static_cast<int>(readDw(L"StartupMechanism",
                                                 RunKeyExists() ? 1 : 0));
    m_startupEnabled   = m_startupMechanism != 0;

    RegCloseKey(key);

    m_gameProfiles = GameProfiles::Load();

    // Persist whatever was migrated and drop the retired values, so each
    // fill-in above runs exactly once and a later choice of "Off" is not
    // resurrected on the next launch.
    if (migrateBackMouse || migratePads) {
        SaveSettings();
        HKEY writeKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_SET_VALUE, &writeKey) == ERROR_SUCCESS) {
            if (migrateBackMouse) RegDeleteValueW(writeKey, L"BackButtons");
            if (migratePads) {
                RegDeleteValueW(writeKey, L"TrackpadMouse");
                RegDeleteValueW(writeKey, L"UseLeftTrackpad");
            }
            RegCloseKey(writeKey);
        }
    }
}

void TrayApp::SaveSettings() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return;

    auto writeDw = [&](const wchar_t* name, DWORD val) {
        RegSetValueExW(key, name, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&val), sizeof(val));
    };

    writeDw(L"AutoSteamMode",       static_cast<DWORD>(m_autoMode));
    writeDw(L"StartupMechanism",    static_cast<DWORD>(m_startupMechanism));
    writeDw(L"ShowNotifications",   m_notificationsEnabled ? 1 : 0);

    // Always the default profile, never ControllerManager's live one: a game
    // profile can be active here, and persisting that would overwrite the
    // user's own settings with a per-game override.
    const auto& profile = m_defaultProfile;
    writeDw(L"ControllerPlatform",
            profile.platform == ControllerPlatform::PlayStation ? 1 : 0);
    writeDw(L"BackBtnL4", profile.back.l4.Pack());
    writeDw(L"BackBtnL5", profile.back.l5.Pack());
    writeDw(L"BackBtnR4", profile.back.r4.Pack());
    writeDw(L"BackBtnR5", profile.back.r5.Pack());

    writeDw(L"LeftPadMode",       static_cast<DWORD>(profile.leftPad.mode));
    writeDw(L"LeftPadClick",      profile.leftPad.click.Pack());
    writeDw(L"LeftPadScrollDir",  static_cast<DWORD>(profile.leftPad.scrollDir));
    writeDw(L"RightPadMode",      static_cast<DWORD>(profile.rightPad.mode));
    writeDw(L"RightPadClick",     profile.rightPad.click.Pack());
    writeDw(L"RightPadScrollDir", static_cast<DWORD>(profile.rightPad.scrollDir));
    // Written last and unconditionally: its presence is what tells the next
    // launch the per-pad values above are authoritative.
    writeDw(L"PadsMigrated", 1);

    RegCloseKey(key);
}

void TrayApp::ShowContextMenu() {
    // Re-enumerate before reading state off the manager. Releasing the
    // controller clears its slot list and only a device event refills it, so
    // after a manual disable the menu grayed out its own toggle as "(no
    // controller detected)" and stayed that way — the controller was sitting
    // right there, and only restarting the app brought the toggle back (#68).
    //
    // Not while a cycle we asked for is running, though: this opens handles,
    // and an open handle is what vetoes a cycle, ours included.
    if (!m_cycleInFlight)
        m_controller->OnDeviceChange();

    bool connected    = m_controller->IsConnected();
    bool gameModeOn   = m_controller->IsGameModeActive();
    bool startupOn    = m_startupEnabled;

    HMENU menu = CreatePopupMenu();

    // Only present when it applies, and first when it does: with the devnode
    // switched off nothing else on this menu can do anything useful, and every
    // other entry will be reporting "no controller detected" for a reason the
    // user has no way to guess at. Re-resolved on each open so it disappears
    // once the device is back, however it got there.
    m_disabledDeviceNode = DeviceRestart::FindDisabledControllerNode();
    if (!m_disabledDeviceNode.empty()) {
        AppendMenuW(menu, MF_STRING, IDM_ENABLE_DEVICE,
                    L"Re-enable Controller Device (disabled in Windows)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    // Manual toggle is disabled while an auto mode owns the decision — when
    // grayed, the label itself says why.
    const bool manual = (m_autoMode == AutoMode::Manual);
    // In manual mode the label tracks intent, not the current handle state:
    // acquiring can spend several seconds cycling the device, and during that
    // window the toggle's job is to offer a cancel.
    const bool toggleOn = manual ? m_wantControl : gameModeOn;
    std::wstring toggleLabel = toggleOn ? L"Disable Steamless Mode"
                                        : L"Enable Steamless Mode";
    if (!connected)
        toggleLabel += L" (no controller detected)";
    else if (!manual)
        toggleLabel += L" (managed by Auto Mode)";
    UINT toggleFlags = MF_STRING | ((connected && manual) ? MF_ENABLED : MF_GRAYED);
    AppendMenuW(menu, toggleFlags, IDM_TOGGLE, toggleLabel.c_str());

    HMENU modeMenu = CreatePopupMenu();
    AppendMenuW(modeMenu, MF_STRING | (manual ? MF_CHECKED : MF_UNCHECKED),
                IDM_MODE_MANUAL, L"Manual");
    AppendMenuW(modeMenu, MF_STRING | (m_autoMode == AutoMode::OffWhileSteam ? MF_CHECKED : MF_UNCHECKED),
                IDM_MODE_STEAM, L"Auto - Off while Steam running");
    AppendMenuW(modeMenu, MF_STRING | (m_autoMode == AutoMode::OffOnlyInGame ? MF_CHECKED : MF_UNCHECKED),
                IDM_MODE_GAME, L"Auto - Off ONLY while in Steam game");
    AppendMenuW(modeMenu, MF_STRING | (m_autoMode == AutoMode::OffUnlessProfile ? MF_CHECKED : MF_UNCHECKED),
                IDM_MODE_PROFILE, L"Auto - Off unless a game profile is running");
    AppendMenuW(menu, MF_STRING | MF_POPUP,
                reinterpret_cast<UINT_PTR>(modeMenu), L"Control Mode");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Everything about how the controller behaves — trackpads, paddles, and
    // which kind of pad the game sees — is configured in one window now, per
    // profile. This menu used to carry three separate entries for pieces of
    // that, none of which could be set per game.
    AppendMenuW(menu, MF_STRING, IDM_REMAP_BACK, L"Customize Controls...");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | (startupOn ? MF_CHECKED : MF_UNCHECKED),
                IDM_STARTUP, L"Start with Windows");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPENLOG, L"Open Event Log");
    AppendMenuW(menu, MF_STRING | (m_notificationsEnabled ? MF_CHECKED : MF_UNCHECKED),
                IDM_NOTIFICATIONS, L"Show Notifications");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    // SetForegroundWindow is required for the menu to dismiss on click-away.
    SetForegroundWindow(m_hwnd);

    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
}
