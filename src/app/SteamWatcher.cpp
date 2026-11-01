#include "SteamWatcher.h"
#include "EventLog.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <cwchar>
#include <string>

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

static bool ReadDword(const wchar_t* subKey, const wchar_t* name, DWORD& out) {
    DWORD size = sizeof(out);
    return RegGetValueW(HKEY_CURRENT_USER, subKey, name, RRF_RT_REG_DWORD,
                        nullptr, &out, &size) == ERROR_SUCCESS;
}

static std::wstring ReadString(const wchar_t* subKey, const wchar_t* name) {
    wchar_t buf[MAX_PATH]{};
    DWORD   size = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, subKey, name, RRF_RT_REG_SZ,
                     nullptr, buf, &size) != ERROR_SUCCESS)
        return L"absent";
    return buf;
}

// Whether the process Steam names as the active one is actually running.
static const wchar_t* ProcessState(DWORD pid) {
    if (pid == 0) return L"none";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED ? L"guarded" : L"gone";
    DWORD      code = 0;
    const bool read = GetExitCodeProcess(h, &code) != FALSE;
    CloseHandle(h);
    return read && code != STILL_ACTIVE ? L"gone" : L"alive";
}

// Steam publishes the app id of the running game in RunningAppID and resets it
// to 0 on exit. Covers Steam games and non-Steam shortcuts launched through
// Steam; games launched entirely outside Steam are invisible to it (acceptable
// — we keep the controller, and the manual toggle remains the escape hatch).
//
// The value alone is not trusted. A cracked game's Steam emulator writes
// RunningAppID itself, so Steam is not involved and has no Steam Input to
// give the controller to (#124). A launch Steam really made also sets the
// per-app Running flag, which the emulator's app has no entry for — so a
// Steam game counts only when that flag is 1. Non-Steam shortcuts get an id
// with the top bit set and no Apps entry, so for those the value stands.
static bool IsGameRunning() {
    static DWORD s_noted = 0;  // watcher thread only; keeps the log to one line per id

    DWORD appId = 0;
    if (!ReadDword(L"Software\\Valve\\Steam", L"RunningAppID", appId) || appId == 0) {
        s_noted = 0;
        return false;
    }

    if (appId & 0x80000000u) {
        if (s_noted != appId)
            EventLog::Write("STEAM: RunningAppID=%lu is a non-Steam shortcut — counting it as a game", appId);
        s_noted = appId;
        return true;
    }

    wchar_t appKey[96];
    swprintf_s(appKey, L"Software\\Valve\\Steam\\Apps\\%lu", appId);
    DWORD running = 0;
    if (ReadDword(appKey, L"Running", running) && running == 1)
        return true;

    if (s_noted != appId)
        EventLog::Write("STEAM: ignoring RunningAppID=%lu — Steam has no running flag for it, "
                        "so Steam did not launch it", appId);
    s_noted = appId;
    return false;
}

// RunningAppID alone cannot tell a game Steam launched from a value something
// else wrote there (a cracked game's emulator, a stale value after a crash).
// Steam's own launch also sets the per-app Running flag and the ActiveProcess
// pid, and an emulator may repoint SteamClientDll, so record all of them
// whenever the state changes — the log is then enough to see which it was.
static void LogSteamRegistry(SteamState state) {
    static constexpr wchar_t kSteam[]  = L"Software\\Valve\\Steam";
    static constexpr wchar_t kActive[] = L"Software\\Valve\\Steam\\ActiveProcess";

    DWORD      appId   = 0;
    const bool haveApp = ReadDword(kSteam, L"RunningAppID", appId);

    std::wstring running = L"n/a";
    if (haveApp && appId != 0) {
        wchar_t appKey[96];
        swprintf_s(appKey, L"Software\\Valve\\Steam\\Apps\\%lu", appId);
        DWORD flag = 0;
        running = ReadDword(appKey, L"Running", flag) ? std::to_wstring(flag) : L"absent";
    }

    DWORD pid = 0;
    ReadDword(kActive, L"pid", pid);

    const std::wstring appText = haveApp ? std::to_wstring(appId) : L"absent";
    EventLog::Write("STEAM: state %d — RunningAppID=%ls ActiveProcess.pid=%lu (%ls) "
                    "Apps\\<id>\\Running=%ls SteamClientDll64=%ls",
                    static_cast<int>(state), appText.c_str(), pid, ProcessState(pid),
                    running.c_str(), ReadString(kActive, L"SteamClientDll64").c_str());
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
    LogSteamRegistry(reported);
    if (m_onChange) m_onChange(reported);

    SteamState pending     = reported;
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
            LogSteamRegistry(reported);
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
                LogSteamRegistry(reported);
                if (m_onChange) m_onChange(reported);
            }
        }
    }
}
