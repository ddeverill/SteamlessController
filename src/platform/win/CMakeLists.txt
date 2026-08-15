# Windows platform backends: HID/SetupAPI device I/O, ViGEmBus virtual
# gamepad, SendInput injection, the registry-backed settings store, and the
# handful of Windows-only mechanisms (device-node cycling, handle scanning)
# that have no Linux equivalent at all — see IDeviceReclaimer's class comment.
add_library(sc_platform_win STATIC
    DeviceRestart.h
    DeviceRestart.cpp
    GameProfiles.h
    GameProfiles.cpp
    HandleFinder.h
    HandleFinder.cpp
    KeyInput.h
    KeyInput.cpp
    ViGEmBusInfo.h
    ViGEmBusInfo.cpp
    WinDeviceMonitor.h
    WinDeviceReclaimer.h
    WinDeviceReclaimer.cpp
    WinForegroundWatcher.h
    WinForegroundWatcher.cpp
    WinGameLibrary.h
    WinGameLibrary.cpp
    WinGameLibraryAdapter.h
    WinGameLibraryAdapter.cpp
    WinHidDevice.h
    WinHidDevice.cpp
    WinInputInjector.h
    WinInputInjector.cpp
    WinOnScreenKeyboard.h
    WinOnScreenKeyboard.cpp
    WinOnScreenKeyboardAdapter.h
    WinOnScreenKeyboardAdapter.cpp
    WinPlatform.h
    WinPlatform.cpp
    WinPlatformPaths.h
    WinPlatformPaths.cpp
    WinRegistryStore.h
    WinRegistryStore.cpp
    WinScheduler.h
    WinScheduler.cpp
    WinSteamWatcher.h
    WinSteamWatcher.cpp
    WinSteamWatcherAdapter.h
    WinVirtualGamepad.h
    WinVirtualGamepad.cpp
)
target_include_directories(sc_platform_win PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(sc_platform_win PUBLIC
    sc_core
    ViGEmClient
    hid
    setupapi
    cfgmgr32
    ole32
    shell32
    shlwapi
    windowsapp
)
# ViGEm/Client.h itself trips /wd4828 (BOM) under /WX, same as the client
# library it wraps — see sc_target_defaults_relaxed's comment.
sc_target_defaults_relaxed(sc_platform_win)
