#include "SteamWatcher.h"
#include <Windows.h>
#include <TlHelp32.h>

static constexpr int POLL_INTERVAL_MS      = 2000;
static constexpr int POLL_SLICE_MS         = 100;  // wake often for fast Stop()
static constexpr int LESS_STEAM_POLL_COUNT = 3;    // ~6s stable before we take over

static bool IsSteamProcessRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Steam publishes the app id of the running game here and resets it to 0 on
// exit. Covers Steam games and non-Steam shortcuts launched through Steam;
// games launched entirely outside Steam are invisible to it (acceptable — we
// keep the controller, and the manual toggle remains the escape hatch).
static bool IsGameRunning() {
    DWORD appId = 0;
    DWORD size  = sizeof(appId);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"RunningAppID",
                     RRF_RT_REG_DWORD, nullptr, &appId, &size) != ERROR_SUCCESS)
        return false;
    return appId != 0;
}

SteamState SteamWatcher::Detect() {
    if (!IsSteamProcessRunning()) return SteamState::NoSteam;
    return IsGameRunning() ? SteamState::InGame : SteamState::SteamIdle;
}

void SteamWatcher::Start(SteamStateFn onChange) {
    Stop();
    m_onChange = std::move(onChange);
    m_running  = true;
    m_thread   = std::thread(&SteamWatcher::PollLoop, this);
}

void SteamWatcher::Stop() {
    if (m_running.exchange(false) && m_thread.joinable())
        m_thread.join();
}

void SteamWatcher::PollLoop() {
    SteamState reported = Detect();
    m_state = reported;
    if (m_onChange) m_onChange(reported);

    SteamState pending      = reported;
    int        pendingPolls = 0;

    while (m_running.load()) {
        for (int waited = 0; waited < POLL_INTERVAL_MS && m_running.load();
             waited += POLL_SLICE_MS)
            Sleep(POLL_SLICE_MS);
        if (!m_running.load()) break;

        const SteamState now = Detect();
        if (now == reported) {
            pendingPolls = 0;
            continue;
        }

        if (static_cast<int>(now) > static_cast<int>(reported)) {
            // More Steam activity (Steam appeared / game launched) — report
            // immediately so the controller is yielded before Steam needs it.
            reported     = now;
            pendingPolls = 0;
            m_state      = reported;
            if (m_onChange) m_onChange(reported);
        } else {
            // Less Steam activity (game quit / Steam gone) — must hold for
            // several consecutive polls so a Steam self-restart or game
            // relaunch doesn't cause ownership flapping.
            if (now != pending) {
                pending      = now;
                pendingPolls = 1;
            } else if (++pendingPolls >= LESS_STEAM_POLL_COUNT) {
                reported     = now;
                pendingPolls = 0;
                m_state      = reported;
                if (m_onChange) m_onChange(reported);
            }
        }
    }
}
