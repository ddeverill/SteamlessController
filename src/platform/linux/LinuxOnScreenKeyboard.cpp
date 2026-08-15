#include "LinuxOnScreenKeyboard.h"
#include "core/EventLog.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// Checked in this order; the first one found on PATH is used. wvkbd is
// listed first as the most common on wlroots-based handheld/tablet setups.
constexpr std::array<const char*, 3> kCandidates = { "wvkbd-mobintl", "squeekboard", "onboard" };

std::string FindOnPath(const char* name) {
    const char* pathEnv = getenv("PATH");
    if (!pathEnv) return {};
    std::string paths = pathEnv;
    size_t start = 0;
    while (start <= paths.size()) {
        const size_t colon = paths.find(':', start);
        const std::string dir = paths.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!dir.empty()) {
            const std::string candidate = dir + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return {};
}

std::string FindAvailable() {
    for (const char* name : kCandidates) {
        std::string path = FindOnPath(name);
        if (!path.empty()) return path;
    }
    return {};
}

}  // namespace

bool LinuxOnScreenKeyboard::Available() const {
    return !FindAvailable().empty();
}

void LinuxOnScreenKeyboard::Toggle() {
    const std::string bin = FindAvailable();
    if (bin.empty()) {
        static bool loggedOnce = false;
        if (!loggedOnce) {
            loggedOnce = true;
            EventLog::Write("OSK: no on-screen keyboard found on PATH (tried wvkbd-mobintl, "
                            "squeekboard, onboard) — the touchKeyboard binding is a no-op here");
        }
        return;
    }

    // Best-effort only: unlike Windows' single well-known on-screen keyboard,
    // there's no universal show/hide toggle across these tools. This
    // launches the keyboard on each press rather than truly toggling
    // visibility — most of them tolerate being invoked while already
    // running (wvkbd exits cleanly if a second instance can't bind).
    //
    // Double-fork so the keyboard process is reparented to init rather than
    // staying a child of this daemon — it may run for the rest of the
    // session, and a long-lived unreaped child (or one this process would
    // have to remember to wait() on later) is not worth the alternative of
    // a detached grandchild.
    const pid_t mid = fork();
    if (mid == 0) {
        if (fork() == 0) {
            setsid();
            execl(bin.c_str(), bin.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(0);
    } else if (mid > 0) {
        int status = 0;
        waitpid(mid, &status, 0);  // the intermediate child exits almost immediately
    }
}
