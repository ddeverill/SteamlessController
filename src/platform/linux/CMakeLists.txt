# Linux platform backends: hidraw, uinput, udev, TOML settings. Deliberately
# depends on nothing beyond libudev + pthreads — no libsystemd (SdNotify is
# hand-rolled in src/daemon), no libhidapi, no toml++ package dependency
# (vendored single header under third_party/tomlplusplus).
add_library(sc_platform_linux STATIC
    FileSettingsStore.h
    FileSettingsStore.cpp
    LinuxDeviceMonitor.h
    LinuxDeviceMonitor.cpp
    LinuxForegroundWatcher.h
    LinuxGameLibrary.h
    LinuxGameLibrary.cpp
    LinuxHidBackend.h
    LinuxHidBackend.cpp
    LinuxHidDevice.h
    LinuxHidDevice.cpp
    LinuxInputInjector.h
    LinuxInputInjector.cpp
    LinuxKeyMap.h
    LinuxKeyMap.cpp
    LinuxOnScreenKeyboard.h
    LinuxOnScreenKeyboard.cpp
    LinuxPlatform.h
    LinuxPlatform.cpp
    LinuxPlatformPaths.h
    LinuxPlatformPaths.cpp
    LinuxScheduler.h
    LinuxScheduler.cpp
    LinuxSteamWatcher.h
    LinuxSteamWatcher.cpp
    LinuxVirtualGamepad.h
    LinuxVirtualGamepad.cpp
    NullDeviceReclaimer.h
)
target_link_libraries(sc_platform_linux PUBLIC sc_core)

find_package(PkgConfig REQUIRED)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
target_link_libraries(sc_platform_linux PUBLIC PkgConfig::UDEV)

target_include_directories(sc_platform_linux PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/tomlplusplus
)
sc_target_defaults(sc_platform_linux)
