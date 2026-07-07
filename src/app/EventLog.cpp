#include "EventLog.h"
#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace {

constexpr long kMaxLogBytes = 512 * 1024;

std::mutex g_mutex;
FILE*      g_file = nullptr;

std::wstring LogDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring dir(buf);
    dir += L"\\SteamlessController";
    return dir;
}

std::wstring LogPath()    { return LogDir() + L"\\events.log"; }
std::wstring OldLogPath() { return LogDir() + L"\\events.old.log"; }

// Called with g_mutex held.
void RotateLocked() {
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    MoveFileExW(LogPath().c_str(), OldLogPath().c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Called with g_mutex held.
bool OpenLocked() {
    if (g_file) return true;
    const std::wstring dir = LogDir();
    if (dir.empty()) return false;
    CreateDirectoryW(dir.c_str(), nullptr);
    // _SH_DENYWR: others may read the log (Notepad via "Open Event Log",
    // support copying it) while we hold it open for append.
    g_file = _wfsopen(LogPath().c_str(), L"a", _SH_DENYWR);
    return g_file != nullptr;
}

}  // namespace

namespace EventLog {

void Write(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!OpenLocked()) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_file, "%04u-%02u-%02u %02u:%02u:%02u.%03u  ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_file, fmt, ap);
    va_end(ap);

    fputc('\n', g_file);
    fflush(g_file);

    if (ftell(g_file) > kMaxLogBytes)
        RotateLocked();  // next Write reopens a fresh file
}

std::wstring FilePath() {
    return LogPath();
}

}  // namespace EventLog
