#include "TrayApp.h"
#include "ControllerManager.h"
#include "ControllerPlatform.h"
#include "DeviceRestart.h"
#include "EventLog.h"
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
    // Stop the watcher before anything else so its callback can't post to a
    // window (or reference a controller) that is being torn down.
    m_steamWatcher.Stop();
    RemoveTrayIcon();
    g_app = nullptr;
}

bool TrayApp::Init(HINSTANCE hInstance) {
    m_hInstance = hInstance;
    m_iconOff   = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_OFF));
    m_iconOn    = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_ON));
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

    EventLog::Write("=== SteamlessController started ===");

    m_controller = std::make_unique<ControllerManager>(
        [this](bool connected, bool gameModeActive, bool vigemMissing) {
            UpdateTrayIcon(connected, gameModeActive, vigemMissing);
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
    UpdateTrayIcon(m_controller->IsConnected(), m_controller->IsGameModeActive(), false);

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

    // Start watching for Steam regardless of auto mode — the state is applied
    // only when auto mode is on, and having it warm makes toggling auto mode
    // take effect instantly. The initial callback asserts the correct state
    // at startup (which also self-heals after a previous crash).
    m_steamWatcher.Start([this](SteamState state) {
        PostMessageW(m_hwnd, WM_STEAMSTATE, static_cast<WPARAM>(state), 0);
    });
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
            else
                ShellExecuteW(nullptr, L"open", L"https://github.com/nefarius/ViGEmBus/releases/latest",
                              nullptr, nullptr, SW_SHOWNORMAL);
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
        case IDM_TRACKPAD:
            m_controller->SetTrackpadMouseEnabled(!m_controller->IsTrackpadMouseEnabled());
            SaveSettings();
            break;
        case IDM_REMAP_BACK:
            OpenRemapWindow();
            break;
        case IDM_LEFT_TRACKPAD:
            m_controller->SetUseLeftTrackpad(!m_controller->IsUseLeftTrackpad());
            SaveSettings();
            break;
        case IDM_PLATFORM_XBOX:
            m_controller->SetControllerPlatform(ControllerPlatform::Xbox);
            SaveSettings();
            break;
        case IDM_PLATFORM_PS:
            m_controller->SetControllerPlatform(ControllerPlatform::PlayStation);
            SaveSettings();
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
        case IDM_STARTUP:
            m_startupEnabled = !m_startupEnabled;
            UpdateStartupRegistration();
            break;
        case IDM_OPENLOG:
            OpenEventLog();
            break;
        case IDM_NOTIFICATIONS:
            m_notificationsEnabled = !m_notificationsEnabled;
            EventLog::Write("USER: disconnect notifications %s",
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
        // Outside ApplySteamState, which returns early in Manual mode — the
        // shared-handle decision needs to know about Steam in every mode.
        m_controller->SetSteamPresent(steamState != SteamState::NoSteam);
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
        } else if (wp == IDT_ACQUIRE_VERDICT) {
            KillTimer(m_hwnd, IDT_ACQUIRE_VERDICT);
            // The last cycle's re-arrival had a grace period to land. If it
            // did, the controller is ours and there is nothing to report.
            if (m_controller->IsGameModeActive()) return 0;
            ReportAcquireFailure();
        }
        return 0;

    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
            m_controller->OnDeviceChange();
            // A controller arriving (plugged in, or re-arriving after a device
            // cycle) should be acquired straight away whenever control is
            // wanted — this is what wins the reopen race against Steam.
            if (wp == DBT_DEVICEARRIVAL && m_wantControl)
                TryAcquireController();
        }
        return TRUE;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void TrayApp::SetAutoMode(AutoMode mode) {
    EventLog::Write("MODE: control mode set to %d (0=manual 1=offWhileSteam 2=offOnlyInGame)",
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
        ApplySteamState(m_steamWatcher.GetState());
}

bool TrayApp::WantControl(SteamState state) const {
    switch (m_autoMode) {
    case AutoMode::OffWhileSteam: return state == SteamState::NoSteam;
    case AutoMode::OffOnlyInGame: return state != SteamState::InGame;
    default:                      return false;  // Manual: tray toggle decides
    }
}

void TrayApp::ApplySteamState(SteamState state) {
    if (m_autoMode == AutoMode::Manual) return;

    EventLog::Write("AUTO: steam state %d (0=none 1=idle 2=inGame) -> %s",
                    static_cast<int>(state),
                    WantControl(state) ? "take control" : "yield");
    if (WantControl(state)) {
        m_wantControl    = true;
        m_acquireRetries = 0;
        TryAcquireController();
    } else {
        ReleaseControl();
    }
}

// Hand the controller back. Shared by the auto modes yielding to Steam and by
// the manual toggle being switched off — in both cases the user is done with
// us and Steam should be able to pick the pad straight back up.
void TrayApp::ReleaseControl() {
    KillTimer(m_hwnd, IDT_ACQUIRE);
    KillTimer(m_hwnd, IDT_ACQUIRE_VERDICT);
    KillTimer(m_hwnd, IDT_WAKE_POLL);
    m_wantControl    = false;
    m_acquireRetries = 0;
    m_lastCycleTick  = 0;
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
    m_controller->OnDeviceChange();
    auto outcome = m_controller->EnableGameMode(stateWaitMs);

    // A short burst of rapid retries: right after a device cycle we race
    // Steam's re-enumeration for the exclusive open, and starting the claim
    // the instant the device arrives is what wins that race.
    for (int i = 0; i < 10 && !m_controller->IsGameModeActive(); ++i) {
        Sleep(50);
        m_controller->OnDeviceChange();
        outcome = m_controller->EnableGameMode(stateWaitMs);
    }
    if (m_controller->IsGameModeActive()) {
        KillTimer(m_hwnd, IDT_ACQUIRE_VERDICT);
        KillTimer(m_hwnd, IDT_WAKE_POLL);
        m_acquireRetries = 0;
        m_lastCycleTick  = 0;
        return;
    }
    if (!m_controller->IsConnected()) return;  // nothing plugged in — arrival will retrigger

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
        return anyRestarted;
    }

    // Normal path: fire the elevated helper via its scheduled task. Starting
    // a task the user authored needs no elevation, so no UAC prompt here.
    std::wstring run = L"schtasks.exe /Run /TN \"";
    run += CYCLE_TASK_NAME;
    run += L"\"";
    if (RunToolHidden(run)) return true;

    // Task missing (copied exes without installing?) — register it with a
    // one-time UAC prompt, then retry.
    if (!EnsureCycleTaskRegistered()) return false;
    return RunToolHidden(std::move(run));
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
    m_remapWindow.Open(
        m_hInstance,
        m_controller.get(),
        m_controller->GetBackButtonConfig(),
        [this](const BackButtonConfig& cfg) {
            m_controller->SetBackButtonConfig(cfg);
            SaveSettings();
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

void TrayApp::UpdateTrayIcon(bool connected, bool gameModeActive, bool vigemMissing) {
    if (vigemMissing) ShowViGEmBalloon();
    else              m_vigemBalloonShown = false;

    // Fall through rather than returning early — the icon and tooltip still need
    // to track the controller while the driver is unavailable.
    const wchar_t* tip = gameModeActive ? L"Steamless Controller — Steamless Mode ON"
                       : vigemMissing   ? L"Steamless Controller — ViGEmBus driver unavailable"
                       : connected      ? L"Steamless Controller — Connected (Steamless Mode OFF)"
                                        : L"Steamless Controller — No controller found";

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_hwnd;
    nid.uID    = TRAY_UID;
    nid.uFlags = NIF_TIP | NIF_ICON;
    nid.hIcon  = gameModeActive ? m_iconOn : m_iconOff;
    wcscpy_s(nid.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayApp::ShowAlertBalloon(const std::wstring& title, const std::wstring& text,
                              BalloonAction action) {
    m_balloonAction = action;

    NOTIFYICONDATAW nid{};
    nid.cbSize      = sizeof(nid);
    nid.hWnd        = m_hwnd;
    nid.uID         = TRAY_UID;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
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
    if (m_vigemBalloonShown) return;
    m_vigemBalloonShown = true;

    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = m_hwnd;
    nid.uID              = TRAY_UID;
    nid.uFlags           = NIF_INFO;
    nid.dwInfoFlags      = NIIF_WARNING;
    wcscpy_s(nid.szInfoTitle, L"Driver required");
    wcscpy_s(nid.szInfo,
             L"ViGEmBus is unavailable — not installed, or disabled in Device "
             L"Manager under System devices. Click here to download it.");
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

    // 0 = Manual, 1 = OffWhileSteam, 2 = OffOnlyInGame (old builds stored 0/1).
    const DWORD mode = readDw(L"AutoSteamMode", 0);
    m_autoMode = mode == 2 ? AutoMode::OffOnlyInGame
               : mode == 1 ? AutoMode::OffWhileSteam
                           : AutoMode::Manual;
    m_controller->SetTrackpadMouseEnabled(readDw(L"TrackpadMouse",   0) != 0);
    m_controller->SetUseLeftTrackpad     (readDw(L"UseLeftTrackpad", 0) != 0);
    m_controller->SetControllerPlatform  (readDw(L"ControllerPlatform", 0) != 0
                                          ? ControllerPlatform::PlayStation
                                          : ControllerPlatform::Xbox);

    // Existing installs keep exactly the bindings they had — the new
    // click-by-default only applies to fresh installs, which never reach here
    // and so take the BackButtonConfig defaults.
    const DWORD unbound = BackButtonBinding::FromAction(BackButtonAction::None).Pack();
    BackButtonConfig cfg;
    cfg.l4 = BackButtonBinding::Unpack(readDw(L"BackBtnL4", unbound));
    cfg.l5 = BackButtonBinding::Unpack(readDw(L"BackBtnL5", unbound));
    cfg.r4 = BackButtonBinding::Unpack(readDw(L"BackBtnR4", unbound));
    cfg.r5 = BackButtonBinding::Unpack(readDw(L"BackBtnR5", unbound));

    // Migrate the retired "Back Buttons as Mouse Click" toggle into real
    // bindings. The toggle used to hijack L4/R4 for a left click and silently
    // discard whatever they were bound to, so only fill in the paddles that
    // have no binding of their own — an explicit mapping was the user's intent
    // all along and starts working again now that nothing overrides it.
    const bool migrateBackMouse = readDw(L"BackButtons", 0) != 0;
    if (migrateBackMouse) {
        const auto leftClick = BackButtonBinding::FromAction(BackButtonAction::LeftMouseButton);
        if (cfg.l4.IsAction(BackButtonAction::None)) cfg.l4 = leftClick;
        if (cfg.r4.IsAction(BackButtonAction::None)) cfg.r4 = leftClick;
    }
    m_controller->SetBackButtonConfig(cfg);

    m_notificationsEnabled = readDw(L"ShowNotifications", 1) != 0;

    // Startup mechanism: 0 none, 1 Run key, 2 elevated task. Migrate installs
    // that predate the setting by probing the Run key they would have used.
    m_startupMechanism = static_cast<int>(readDw(L"StartupMechanism",
                                                 RunKeyExists() ? 1 : 0));
    m_startupEnabled   = m_startupMechanism != 0;

    RegCloseKey(key);

    // Persist the migrated bindings and drop the retired value, so the fill-in
    // above runs exactly once and a later choice of "Off" is not resurrected.
    if (migrateBackMouse) {
        SaveSettings();
        HKEY writeKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_SET_VALUE, &writeKey) == ERROR_SUCCESS) {
            RegDeleteValueW(writeKey, L"BackButtons");
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
    writeDw(L"TrackpadMouse",       m_controller->IsTrackpadMouseEnabled()  ? 1 : 0);
    writeDw(L"UseLeftTrackpad",     m_controller->IsUseLeftTrackpad()       ? 1 : 0);
    writeDw(L"ControllerPlatform",  m_controller->GetControllerPlatform() == ControllerPlatform::PlayStation ? 1 : 0);

    const auto& cfg = m_controller->GetBackButtonConfig();
    writeDw(L"BackBtnL4", cfg.l4.Pack());
    writeDw(L"BackBtnL5", cfg.l5.Pack());
    writeDw(L"BackBtnR4", cfg.r4.Pack());
    writeDw(L"BackBtnR5", cfg.r5.Pack());

    RegCloseKey(key);
}

void TrayApp::ShowContextMenu() {
    bool connected    = m_controller->IsConnected();
    bool gameModeOn   = m_controller->IsGameModeActive();
    bool trackpadOn   = m_controller->IsTrackpadMouseEnabled();
    bool leftPad      = m_controller->IsUseLeftTrackpad();
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
    AppendMenuW(menu, MF_STRING | MF_POPUP,
                reinterpret_cast<UINT_PTR>(modeMenu), L"Control Mode");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | (trackpadOn ? MF_CHECKED : MF_UNCHECKED),
                IDM_TRACKPAD, L"Enable Trackpad Mouse");

    AppendMenuW(menu, MF_STRING | (leftPad ? MF_CHECKED : MF_UNCHECKED),
                IDM_LEFT_TRACKPAD, L"Use Left Trackpad Instead");

    AppendMenuW(menu, MF_STRING, IDM_REMAP_BACK, L"Remap Back Buttons...");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    bool isPS = (m_controller->GetControllerPlatform() == ControllerPlatform::PlayStation);
    HMENU platformMenu = CreatePopupMenu();
    AppendMenuW(platformMenu, MF_STRING | (!isPS ? MF_CHECKED : MF_UNCHECKED),
                IDM_PLATFORM_XBOX, L"Xbox");
    AppendMenuW(platformMenu, MF_STRING | (isPS ? MF_CHECKED : MF_UNCHECKED),
                IDM_PLATFORM_PS, L"PlayStation");
    AppendMenuW(menu, MF_STRING | MF_POPUP,
                reinterpret_cast<UINT_PTR>(platformMenu), L"Controller Platform");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | (startupOn ? MF_CHECKED : MF_UNCHECKED),
                IDM_STARTUP, L"Start with Windows");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPENLOG, L"Open Event Log");
    AppendMenuW(menu, MF_STRING | (m_notificationsEnabled ? MF_CHECKED : MF_UNCHECKED),
                IDM_NOTIFICATIONS, L"Disconnect Notifications");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    // SetForegroundWindow is required for the menu to dismiss on click-away.
    SetForegroundWindow(m_hwnd);

    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
}
