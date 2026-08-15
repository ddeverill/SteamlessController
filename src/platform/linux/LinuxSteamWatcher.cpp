#include "LinuxSteamWatcher.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

constexpr int POLL_INTERVAL_MS      = 2000;
constexpr int POLL_SLICE_MS         = 100;  // wake often for fast Stop()
constexpr int LESS_STEAM_POLL_COUNT = 3;    // ~6s stable before we take over

std::string HomeDir() {
    if (const char* h = getenv("HOME")) return h;
    return "";
}

// registry.vdf's SteamPID line looks like: "SteamPID"    "48434" — a plain
// substring scan for the key and the next quoted token after it is enough
// for this one value, without pulling in the full VDF tokenizer
// SteamAppLocator uses for the much larger library/manifest files.
long ReadSteamPid(const std::string& path) {
    std::ifstream f(path);
    if (!f) return -1;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    const size_t keyPos = text.find("\"SteamPID\"");
    if (keyPos == std::string::npos) return -1;
    const size_t firstQuote = text.find('"', keyPos + 10);
    if (firstQuote == std::string::npos) return -1;
    const size_t secondQuote = text.find('"', firstQuote + 1);
    if (secondQuote == std::string::npos) return -1;
    return std::atol(text.substr(firstQuote + 1, secondQuote - firstQuote - 1).c_str());
}

bool ProcessAlive(long pid) {
    return pid > 0 && access(("/proc/" + std::to_string(pid)).c_str(), F_OK) == 0;
}

// Fallback for when registry.vdf can't be read at all: scan /proc for a
// process whose comm is exactly "steam" (the launcher and steamwebhelper
// children share pieces of that name but not the exact comm).
bool AnySteamProcessInProc() {
    DIR* proc = opendir("/proc");
    if (!proc) return false;
    bool found = false;
    while (struct dirent* entry = readdir(proc)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        std::ifstream comm("/proc/" + std::string(entry->d_name) + "/comm");
        std::string name;
        if (comm && std::getline(comm, name) && name == "steam") { found = true; break; }
    }
    closedir(proc);
    return found;
}

bool IsSteamProcessRunning() {
    const std::string home = HomeDir();
    for (const std::string& registryPath : {
             home + "/.steam/registry.vdf",
             home + "/.var/app/com.valvesoftware.Steam/.steam/registry.vdf",
         }) {
        const long pid = ReadSteamPid(registryPath);
        if (pid > 0) return ProcessAlive(pid);
        if (pid == 0) return false;  // Steam wrote "0" — explicitly not running
    }
    return AnySteamProcessInProc();
}

// Steam exports SteamAppId into every game process it launches (native,
// Proton, or a non-Steam shortcut run through it) — readable via
// /proc/<pid>/environ for any same-uid process. Games launched entirely
// outside Steam are invisible to this, same limitation as the Windows
// RunningAppID registry check has.
bool IsGameRunning() {
    DIR* proc = opendir("/proc");
    if (!proc) return false;
    bool found = false;
    while (struct dirent* entry = readdir(proc)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        std::ifstream environ("/proc/" + std::string(entry->d_name) + "/environ", std::ios::binary);
        if (!environ) continue;
        std::string data((std::istreambuf_iterator<char>(environ)), std::istreambuf_iterator<char>());
        // environ entries are NUL-separated "KEY=VALUE" strings.
        if (data.find("SteamAppId=") != std::string::npos ||
            data.find("SteamGameId=") != std::string::npos) {
            found = true;
            break;
        }
    }
    closedir(proc);
    return found;
}

}  // namespace

SteamState LinuxSteamWatcher::Detect() {
    if (!IsSteamProcessRunning()) return SteamState::NoSteam;
    return IsGameRunning() ? SteamState::InGame : SteamState::SteamIdle;
}

void LinuxSteamWatcher::Start(SteamStateFn onChange) {
    Stop();
    m_onChange = std::move(onChange);
    m_running  = true;
    m_thread   = std::thread(&LinuxSteamWatcher::PollLoop, this);
}

void LinuxSteamWatcher::Stop() {
    if (m_running.exchange(false) && m_thread.joinable())
        m_thread.join();
}

void LinuxSteamWatcher::PollLoop() {
    SteamState reported = Detect();
    m_state = reported;
    if (m_onChange) m_onChange(reported);

    SteamState pending      = reported;
    int        pendingPolls = 0;

    while (m_running.load()) {
        for (int waited = 0; waited < POLL_INTERVAL_MS && m_running.load(); waited += POLL_SLICE_MS)
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_SLICE_MS));
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
