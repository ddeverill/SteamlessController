#include "EventLog.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

namespace {

constexpr long kMaxLogBytes = 512 * 1024;

std::mutex  g_mutex;
FILE*       g_file = nullptr;
std::string g_dir;

std::string LogPath()    { return g_dir + "/events.log"; }
std::string OldLogPath() { return g_dir + "/events.old.log"; }

// Called with g_mutex held.
void RotateLocked() {
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    std::error_code ec;
    fs::rename(LogPath(), OldLogPath(), ec);  // best-effort; a failure just means no rotation this time
}

// Called with g_mutex held.
bool OpenLocked() {
    if (g_file) return true;
    if (g_dir.empty()) return false;
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    // Append mode: another reader (a text editor, support copying the file)
    // can open it concurrently without disturbing our writes.
    g_file = fopen(LogPath().c_str(), "a");
    return g_file != nullptr;
}

}  // namespace

namespace EventLog {

void Init(const std::string& dir) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) { fclose(g_file); g_file = nullptr; }
    g_dir = dir;
}

void Write(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!OpenLocked()) return;

    const auto now  = std::chrono::system_clock::now();
    const auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()) % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    fprintf(g_file, "%04d-%02d-%02d %02d:%02d:%02d.%03lld  ",
            local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
            local.tm_hour, local.tm_min, local.tm_sec,
            static_cast<long long>(ms.count()));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_file, fmt, ap);
    va_end(ap);

    fputc('\n', g_file);
    fflush(g_file);

    if (ftell(g_file) > kMaxLogBytes)
        RotateLocked();  // next Write reopens a fresh file
}

std::string FilePath() {
    return LogPath();
}

}  // namespace EventLog
