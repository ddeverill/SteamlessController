#pragma once
#include <functional>

class IHidBackend;
class IVirtualGamepadFactory;
class IInputInjector;
class ISettingsStore;
class IForegroundWatcher;
class ISteamWatcher;
class IGameLibrary;
class IDeviceMonitor;
class IDeviceReclaimer;
class IOnScreenKeyboard;
class IPlatformPaths;
class IScheduler;

// The one object core/ code is handed. WinPlatform and LinuxPlatform own the
// concrete backends and are constructed once, at process startup, by
// src/win/main.cpp / src/daemon/main.cpp respectively.
class IPlatform {
public:
    virtual ~IPlatform() = default;

    virtual IHidBackend&            Hid()        = 0;
    virtual IVirtualGamepadFactory& Gamepads()   = 0;
    virtual IInputInjector&         Input()      = 0;
    virtual ISettingsStore&         Settings()   = 0;
    virtual IForegroundWatcher&     Foreground() = 0;
    virtual ISteamWatcher&          Steam()      = 0;
    virtual IGameLibrary&           Games()      = 0;
    virtual IDeviceMonitor&         Devices()    = 0;
    virtual IDeviceReclaimer&       Reclaimer()  = 0;
    virtual IOnScreenKeyboard&      Osk()        = 0;
    virtual IPlatformPaths&         Paths()      = 0;
    virtual IScheduler&             Scheduler()  = 0;

    // Installs the process-wide crash/signal handler that must still be able
    // to call emergencyRestore() (SteamController::EmergencyLizardRestore)
    // with no locks held and no threads joined. Windows: SetUnhandledExceptionFilter.
    // Linux: SIGTERM/SIGINT/SIGHUP for graceful shutdown, SIGSEGV/SIGABRT for
    // the async-signal-safe emergency path only.
    virtual void InstallCrashHandler(std::function<void()> emergencyRestore) = 0;
};
