#pragma once
#include "core/iface/IPlatform.h"
#include "FileSettingsStore.h"
#include "LinuxDeviceMonitor.h"
#include "LinuxForegroundWatcher.h"
#include "LinuxGameLibrary.h"
#include "LinuxHidBackend.h"
#include "LinuxInputInjector.h"
#include "LinuxOnScreenKeyboard.h"
#include "LinuxPlatformPaths.h"
#include "LinuxScheduler.h"
#include "LinuxSteamWatcher.h"
#include "LinuxVirtualGamepad.h"
#include "NullDeviceReclaimer.h"

// Owns every Linux backend and hands them out through IPlatform. Constructed
// once at daemon startup (src/daemon/main.cpp).
class LinuxPlatform : public IPlatform {
public:
    LinuxPlatform();

    IHidBackend&            Hid()        override { return m_hid; }
    IVirtualGamepadFactory& Gamepads()   override { return m_gamepads; }
    IInputInjector&         Input()      override { return m_input; }
    ISettingsStore&         Settings()   override { return *m_settings; }
    IForegroundWatcher&     Foreground() override { return m_foreground; }
    ISteamWatcher&          Steam()      override { return m_steam; }
    IGameLibrary&           Games()      override { return m_games; }
    IDeviceMonitor&         Devices()    override { return m_devices; }
    IDeviceReclaimer&       Reclaimer()  override { return m_reclaimer; }
    IOnScreenKeyboard&      Osk()        override { return m_osk; }
    IPlatformPaths&         Paths()      override { return m_paths; }
    IScheduler&             Scheduler()  override { return m_scheduler; }

    // The concrete type, for Daemon::Run()'s pump loop (RunDuePending() is
    // deliberately not part of the abstract IScheduler interface — see
    // LinuxScheduler's class comment).
    LinuxScheduler& GetScheduler() { return m_scheduler; }

    void InstallCrashHandler(std::function<void()> emergencyRestore) override;

private:
    LinuxPlatformPaths            m_paths;
    LinuxHidBackend               m_hid;
    LinuxVirtualGamepadFactory    m_gamepads;
    LinuxInputInjector            m_input;
    std::unique_ptr<FileSettingsStore> m_settings;
    LinuxForegroundWatcher        m_foreground;
    LinuxSteamWatcher             m_steam;
    LinuxGameLibrary              m_games;
    LinuxDeviceMonitor            m_devices;
    NullDeviceReclaimer           m_reclaimer;
    LinuxOnScreenKeyboard         m_osk;
    LinuxScheduler                m_scheduler;
};
