# Platform-neutral core: protocol logic, settings model, controller
# orchestration. Zero OS headers — compiles unchanged with MSVC or GCC/Clang.
add_library(sc_core STATIC
    AppSettings.h
    BackButtonConfig.h
    BindingText.h
    BindingText.cpp
    ControllerManager.h
    ControllerManager.cpp
    ControllerPlatform.h
    EventLog.h
    EventLog.cpp
    Format.h
    KeyNames.h
    KeyNames.cpp
    ModifierTracker.h
    ModifierTracker.cpp
    SettingsCodec.h
    SettingsCodec.cpp
    SteamAppLocator.h
    SteamAppLocator.cpp
    SteamController.h
    SteamController.cpp
    Text.h
    Text.cpp
    TrackpadConfig.h
    TrackpadMouse.h
    TrackpadMouse.cpp
    iface/IDeviceMonitor.h
    iface/IDeviceReclaimer.h
    iface/IForegroundWatcher.h
    iface/IGameLibrary.h
    iface/IHidDevice.h
    iface/IInputInjector.h
    iface/IOnScreenKeyboard.h
    iface/IPlatform.h
    iface/IPlatformPaths.h
    iface/IScheduler.h
    iface/ISettingsStore.h
    iface/ISteamWatcher.h
    iface/IVirtualGamepad.h
)
target_include_directories(sc_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
find_package(Threads REQUIRED)
target_link_libraries(sc_core PUBLIC Threads::Threads)
sc_target_defaults(sc_core)
