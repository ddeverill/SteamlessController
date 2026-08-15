#include "Daemon.h"
#include "platform/linux/LinuxPlatform.h"
#include <csignal>
#include <cstdio>

namespace {
Daemon* g_daemon = nullptr;

extern "C" void HandleShutdownSignal(int) {
    // Graceful path: just flip the run flag. The daemon's own destructors
    // (ControllerManager's in particular) do the actual lizard-mode
    // restore and device cleanup once Run() returns — no emergency-path
    // logic needed here, unlike LinuxPlatform::InstallCrashHandler's
    // SIGSEGV/SIGABRT handler.
    if (g_daemon) g_daemon->RequestStop();
}
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);

    struct sigaction sa{};
    sa.sa_handler = HandleShutdownSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGHUP,  &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);  // a client disconnecting mid-write must not kill the daemon

    LinuxPlatform platform;
    Daemon daemon(platform);
    g_daemon = &daemon;

    daemon.Run();
    return 0;
}
