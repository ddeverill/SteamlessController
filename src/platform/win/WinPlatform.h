#pragma once
#include "core/iface/IPlatform.h"
#include "WinDeviceMonitor.h"
#include "WinDeviceReclaimer.h"
#include "WinForegroundWatcher.h"
#include "WinGameLibraryAdapter.h"
#include "WinHidDevice.h"
#include "WinInputInjector.h"
#include "WinOnScreenKeyboardAdapter.h"
#include "WinPlatformPaths.h"
#include "WinRegistryStore.h"
#include "WinScheduler.h"
#include "WinSteamWatcherAdapter.h"
#include "WinVirtualGamepad.h"

// Owns every Windows backend and hands them out through IPlatform, so
// ControllerManager (cross-platform) can be constructed on Windows exactly
// as it is on Linux.
//
// Only Hid(), Gamepads(), Input(), Osk(), and InstallCrashHandler() are
// actually exercised by ControllerManager today — Settings()/Foreground()/
// Steam()/Games()/Devices()/Reclaimer()/Paths()/Scheduler() exist for
// interface completeness (IPlatform is all pure virtual; a concrete
// WinPlatform has to answer every one of them) but TrayApp still owns and
// drives its own SteamWatcher, WinForegroundWatcher, GameProfiles,
// SteamAppLocator, and DeviceRestart calls directly, unchanged from before
// the cross-platform port. Routing all of that through IPlatform as well —
// so Windows and Linux share one ControllerService instead of TrayApp
// carrying its own copy of the acquire/retry/profile-matching policy — is
// the ControllerService extraction called out as follow-up work; this class
// is deliberately scoped to "make the shared core buildable and correct on
// Windows again" rather than that larger rewrite.
class WinPlatform : public IPlatform {
public:
    WinPlatform();

    IHidBackend&            Hid()        override { return m_hid; }
    IVirtualGamepadFactory& Gamepads()   override { return m_gamepads; }
    IInputInjector&         Input()      override { return m_input; }
    ISettingsStore&         Settings()   override { return m_settings; }
    IForegroundWatcher&     Foreground() override { return m_foreground; }
    ISteamWatcher&          Steam()      override { return m_steam; }
    IGameLibrary&           Games()      override { return m_games; }
    IDeviceMonitor&         Devices()    override { return m_devices; }
    IDeviceReclaimer&       Reclaimer()  override { return m_reclaimer; }
    IOnScreenKeyboard&      Osk()        override { return m_osk; }
    IPlatformPaths&         Paths()      override { return m_paths; }
    IScheduler&             Scheduler()  override { return m_scheduler; }

    void InstallCrashHandler(std::function<void()> emergencyRestore) override;

    // Concrete accessors TrayApp needs beyond the abstract interface: the
    // alert callback (WinInputInjector-specific, not part of IInputInjector)
    // and raw HID enumeration (for TrayApp::RestartControllerDevices, which
    // predates and works independently of ControllerManager's own use of
    // IHidBackend).
    WinInputInjector& GetInput() { return m_input; }
    WinHidBackend&    GetHid()   { return m_hid; }

private:
    WinHidBackend               m_hid;
    WinVirtualGamepadFactory    m_gamepads;
    WinInputInjector            m_input;
    WinRegistryStore            m_settings;
    WinForegroundWatcher        m_foreground;
    WinSteamWatcherAdapter      m_steam;
    WinGameLibraryAdapter       m_games;
    WinDeviceMonitor            m_devices;
    WinDeviceReclaimer          m_reclaimer;
    WinOnScreenKeyboardAdapter  m_osk;
    WinPlatformPaths            m_paths;
    WinScheduler                m_scheduler;
};
