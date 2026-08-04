#include "TrayApp.h"
#include "ControllerManager.h"
#include "ControllerPlatform.h"
#include "DeviceRestart.h"
#include "EventLog.h"
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
    m_controller->SetAlertCallback(
        [this](const std::wstring& title, const std::wstring& text) {
            {
                std::lock_guard<std::mutex> lk(m_alertMutex);
                m_alertTitle = title;
                m_alertText  = text;
            }
            PostMessageW(m_hwnd, WM_ALERT, 0, 0);
        });

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
        if (LOWORD(lp) == NIN_BALLOONUSERCLICK)
            ShellExecuteW(nullptr, L"open", L"https://github.com/nefarius/ViGEmBus/releases/latest",
                          nullptr, nullptr, SW_SHOWNORMAL);
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK)
            OpenRemapWindow();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP)
            ShowContextMenu();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_TOGGLE:
            if (m_controller->IsGameModeActive())
                m_controller->DisableGameMode();
            else
                m_controller->EnableGameMode();
            break;
        case IDM_TRACKPAD:
            m_controller->SetTrackpadMouseEnabled(!m_controller->IsTrackpadMouseEnabled());
            SaveSettings();
            break;
        case IDM_REMAP_BACK:
            OpenRemapWindow();
            break;
        case IDM_BACKBUTTONS:
            m_controller->SetBackButtonsEnabled(!m_controller->IsBackButtonsEnabled());
            SaveSettings();
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
            if (m_autoMode != AutoMode::Manual && WantControl(m_steamWatcher.GetState()))
                TryAcquireController();
        } else if (wp == IDT_WAKE_POLL) {
            KillTimer(m_hwnd, IDT_WAKE_POLL);
            // Cheap probe — we are only asking whether anything woke up.
            if (m_autoMode != AutoMode::Manual && WantControl(m_steamWatcher.GetState()))
                TryAcquireController(WAKE_PROBE_MS);
        } else if (wp == IDT_ACQUIRE_VERDICT) {
            KillTimer(m_hwnd, IDT_ACQUIRE_VERDICT);
            // The last cycle's re-arrival had a grace period to land. If it
            // did, the controller is ours and there is nothing to report.
            if (m_controller->IsGameModeActive()) return 0;
            EventLog::Write("AUTO: giving up acquiring after %d device cycles", MAX_ACQUIRE_CYCLES);
            if (m_notificationsEnabled)
                ShowAlertBalloon(L"Could not take the controller",
                                 L"Steam is holding the controller and did not release it. "
                                 L"Closing Steam will hand it back.");
        }
        return 0;

    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
            m_controller->OnDeviceChange();
            // A controller arriving (plugged in, or re-arriving after a device
            // cycle) should be acquired without a manual toggle when the
            // active auto mode wants control right now.
            if (wp == DBT_DEVICEARRIVAL && m_autoMode != AutoMode::Manual
                    && WantControl(m_steamWatcher.GetState()))
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
    if (mode != AutoMode::Manual)
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
        m_acquireRetries = 0;
        TryAcquireController();
    } else {
        KillTimer(m_hwnd, IDT_ACQUIRE);
        KillTimer(m_hwnd, IDT_ACQUIRE_VERDICT);
        KillTimer(m_hwnd, IDT_WAKE_POLL);
        m_acquireRetries = 0;
        m_lastCycleTick  = 0;
        const bool hadControl = m_controller->IsGameModeActive();
        // Good citizen: restore lizard mode and close our HID handles so
        // Steam can claim the controller without contention.
        m_controller->ReleaseDevices();
        // Steam only (re)opens controllers on device-arrival events. If it is
        // already running and we held the device, it never saw one — cycle
        // the device so Steam adopts it immediately.
        if (hadControl && state != SteamState::NoSteam)
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
        m_acquireRetries = 0;
        m_lastCycleTick  = 0;
        // Having one controller does not mean we are done watching. A receiver
        // publishes every slot interface permanently, so a second controller
        // switching on raises no device event either — without this, player two
        // never gets picked up. Slower cadence than when we have nothing, since
        // this is opportunistic rather than something the user is waiting on.
        if (m_controller->HasInactiveSlot())
            SetTimer(m_hwnd, IDT_WAKE_POLL, WAKE_POLL_ACTIVE_MS, nullptr);
        else
            KillTimer(m_hwnd, IDT_WAKE_POLL);
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
        SetTimer(m_hwnd, IDT_WAKE_POLL, WAKE_POLL_IDLE_MS, nullptr);
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
    // Capture the contested interfaces before ReleaseDevices clears the slots.
    // Cycling only these leaves any other controller untouched, and on a
    // multi-slot receiver skips three empty slots' worth of teardown.
    const auto targets = m_controller->BlockedSlotPaths();
    EventLog::Write("AUTO: exclusive claim blocked — cycling %zu device(s) (attempt %d)",
                    targets.size(), m_acquireRetries);
    m_controller->ReleaseDevices();
    DeviceRestart::WriteCycleTargets(targets);
    if (!RestartControllerDevices()) return;  // helper unavailable — balloon shown
    // The cycle runs asynchronously via the helper task; the device-arrival
    // notification drives the claim, with this timer as the fallback. It must
    // outlast a cycle: the helper waits a second between disable and enable,
    // then a multi-slot receiver has to re-enumerate every interface.
    SetTimer(m_hwnd, IDT_ACQUIRE, ACQUIRE_RETRY_MS, nullptr);
}

bool TrayApp::RestartControllerDevices() {
    // On the rare elevated run, cycle directly. This works on every transport
    // including Bluetooth: the devnode being restarted is the HID child, not
    // the pairing (which lives up at the BTHLEDEVICE layer), and it
    // re-enumerates as fast as a USB replug.
    if (IsProcessElevated()) {
        auto paths = DeviceRestart::ConsumeCycleTargets();
        if (paths.empty()) paths = SteamController::EnumerateAll();
        bool anyRestarted = false;
        for (const auto& path : paths)
            if (DeviceRestart::RestartInterfaceDevice(path))
                anyRestarted = true;
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
    if (vigemMissing) { ShowViGEmBalloon(); return; }

    const wchar_t* tip = gameModeActive ? L"Steamless Controller — Steamless Mode ON"
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

void TrayApp::ShowAlertBalloon(const std::wstring& title, const std::wstring& text) {
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

void TrayApp::ShowViGEmBalloon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = m_hwnd;
    nid.uID              = TRAY_UID;
    nid.uFlags           = NIF_INFO;
    nid.dwInfoFlags      = NIIF_WARNING;
    wcscpy_s(nid.szInfoTitle, L"Driver required");
    wcscpy_s(nid.szInfo,      L"ViGEmBus is not installed. Click here to download it.");
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
    m_controller->SetBackButtonsEnabled  (readDw(L"BackButtons",     0) != 0);
    m_controller->SetUseLeftTrackpad     (readDw(L"UseLeftTrackpad", 0) != 0);
    m_controller->SetControllerPlatform  (readDw(L"ControllerPlatform", 0) != 0
                                          ? ControllerPlatform::PlayStation
                                          : ControllerPlatform::Xbox);

    BackButtonConfig cfg;
    cfg.l4 = static_cast<BackButtonAction>(readDw(L"BackBtnL4", static_cast<DWORD>(BackButtonAction::None)));
    cfg.l5 = static_cast<BackButtonAction>(readDw(L"BackBtnL5", static_cast<DWORD>(BackButtonAction::None)));
    cfg.r4 = static_cast<BackButtonAction>(readDw(L"BackBtnR4", static_cast<DWORD>(BackButtonAction::None)));
    cfg.r5 = static_cast<BackButtonAction>(readDw(L"BackBtnR5", static_cast<DWORD>(BackButtonAction::None)));
    m_controller->SetBackButtonConfig(cfg);

    m_notificationsEnabled = readDw(L"ShowNotifications", 1) != 0;

    // Startup mechanism: 0 none, 1 Run key, 2 elevated task. Migrate installs
    // that predate the setting by probing the Run key they would have used.
    m_startupMechanism = static_cast<int>(readDw(L"StartupMechanism",
                                                 RunKeyExists() ? 1 : 0));
    m_startupEnabled   = m_startupMechanism != 0;

    RegCloseKey(key);
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
    writeDw(L"BackButtons",         m_controller->IsBackButtonsEnabled()    ? 1 : 0);
    writeDw(L"UseLeftTrackpad",     m_controller->IsUseLeftTrackpad()       ? 1 : 0);
    writeDw(L"ControllerPlatform",  m_controller->GetControllerPlatform() == ControllerPlatform::PlayStation ? 1 : 0);

    const auto& cfg = m_controller->GetBackButtonConfig();
    writeDw(L"BackBtnL4", static_cast<DWORD>(cfg.l4));
    writeDw(L"BackBtnL5", static_cast<DWORD>(cfg.l5));
    writeDw(L"BackBtnR4", static_cast<DWORD>(cfg.r4));
    writeDw(L"BackBtnR5", static_cast<DWORD>(cfg.r5));

    RegCloseKey(key);
}

void TrayApp::ShowContextMenu() {
    bool connected    = m_controller->IsConnected();
    bool gameModeOn   = m_controller->IsGameModeActive();
    bool trackpadOn   = m_controller->IsTrackpadMouseEnabled();
    bool backMouseOn  = m_controller->IsBackButtonsEnabled();
    bool leftPad      = m_controller->IsUseLeftTrackpad();
    bool startupOn    = m_startupEnabled;

    HMENU menu = CreatePopupMenu();

    // Manual toggle is disabled while an auto mode owns the decision — when
    // grayed, the label itself says why.
    const bool manual = (m_autoMode == AutoMode::Manual);
    std::wstring toggleLabel = gameModeOn ? L"Disable Steamless Mode"
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

    AppendMenuW(menu, MF_STRING | (backMouseOn ? MF_CHECKED : MF_UNCHECKED),
                IDM_BACKBUTTONS, L"Back Buttons as Mouse Click");

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
