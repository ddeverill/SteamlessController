#include "LinuxPlatform.h"
#include <csignal>
#include <cstdlib>

namespace {
// Signal handlers can only reach a plain function pointer, not a capturing
// lambda or std::function member — a single process-wide slot is enough
// since there is exactly one LinuxPlatform per daemon process, the same
// one-instance assumption ControllerManager's own crash-restore global makes.
std::function<void()>* g_emergencyRestore = nullptr;

extern "C" void HandleFatalSignal(int sig) {
    if (g_emergencyRestore && *g_emergencyRestore) (*g_emergencyRestore)();
    // Restore the default handler and re-raise so the process still dies
    // with the expected signal/core dump — this handler's only job is to
    // get a best-effort lizard-mode restore in first.
    signal(sig, SIG_DFL);
    raise(sig);
}
}  // namespace

LinuxPlatform::LinuxPlatform()
    : m_settings(std::make_unique<FileSettingsStore>(m_paths.ConfigDir() + "/config.toml"))
    , m_games(m_paths)
{
}

void LinuxPlatform::InstallCrashHandler(std::function<void()> emergencyRestore) {
    static std::function<void()> holder;  // outlives this call, process-lifetime
    holder = std::move(emergencyRestore);
    g_emergencyRestore = &holder;

    struct sigaction sa{};
    sa.sa_handler = HandleFatalSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    // Crash paths only — SIGTERM/SIGINT/SIGHUP are graceful shutdown, which
    // the daemon's own main loop handles by exiting its poll loop and
    // letting ControllerManager's normal destructor run (which already
    // calls EnableLizardMode cleanly, no emergency path needed).
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}
