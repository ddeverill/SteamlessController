#include "GameLiveness.h"
#include "EventLog.h"

void GameLiveness::Hold(const std::wstring& profileId, DWORD pid) {
    Clear();
    if (profileId.empty() || pid == 0) return;

    m_profileId = profileId;
    m_pid       = pid;
    // SYNCHRONIZE is what makes the cheap check below possible. Asked for on
    // its own rather than alongside query rights, so a process that would
    // grant one and not the other still gives us the one we need.
    m_handle = OpenProcess(SYNCHRONIZE, FALSE, pid);

    EventLog::Write("PROFILE: holding the pad type for %ls (pid %lu, liveness by %s)",
                    profileId.c_str(), pid, m_handle ? "handle" : "pid");
}

void GameLiveness::Clear() {
    if (m_handle) {
        CloseHandle(m_handle);
        m_handle = nullptr;
    }
    m_profileId.clear();
    m_pid = 0;
}

bool GameLiveness::StillRunning() const {
    if (m_profileId.empty()) return false;

    if (m_handle)
        return WaitForSingleObject(m_handle, 0) == WAIT_TIMEOUT;

    // No handle, so ask for the least that could possibly be granted.
    HANDLE probe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, m_pid);
    if (probe) {
        // Opening it proves the process OBJECT exists, which is not the same
        // as the program running: a pid stays valid for as long as anything
        // holds a handle to the process, and a launcher or an anti-cheat
        // service generally does. Measured against Fortnite, an exited game
        // stayed openable indefinitely and a hold that trusted the open never
        // released. Ask whether it has exited instead.
        DWORD      code = 0;
        const bool read = GetExitCodeProcess(probe, &code) != FALSE;
        CloseHandle(probe);
        // Unreadable exit code: assume it is running. Being wrong that way
        // costs the improvement this class exists for; being wrong the other
        // way rebuilds the pad under a game that is still going.
        return read ? code == STILL_ACTIVE : true;
    }
    // Denied means it is there and guarded. Anything else — an invalid
    // parameter, most often — means the id no longer names a process.
    return GetLastError() == ERROR_ACCESS_DENIED;
}
