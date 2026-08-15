# Diagnostic probe tool (console): full Steam Controller HID discovery/dump.
add_executable(SteamProbe
    SteamProbe.cpp
)
target_link_libraries(SteamProbe PRIVATE sc_core sc_platform_win)
sc_target_defaults(SteamProbe)
sc_console_target(SteamProbe)

# RawGameController diagnostic probe (WinRT) — standalone, no sc_core/sc_platform_win dependency.
add_executable(RawControllerProbe
    RawControllerProbe.cpp
)
target_link_libraries(RawControllerProbe PRIVATE windowsapp)
if(MSVC)
    target_compile_options(RawControllerProbe PRIVATE /W4 /utf-8)
endif()

# Game-library diagnostic probe — dumps what the Start Menu walk finds.
add_executable(GameLibraryProbe
    GameLibraryProbe.cpp
)
target_link_libraries(GameLibraryProbe PRIVATE sc_platform_win)
sc_target_defaults(GameLibraryProbe)
sc_console_target(GameLibraryProbe)

# Foreground-watcher diagnostic probe — prints what the per-game profile
# matcher would see as the user switches applications.
add_executable(ForegroundProbe
    ForegroundProbe.cpp
)
target_link_libraries(ForegroundProbe PRIVATE sc_platform_win)
sc_target_defaults(ForegroundProbe)
sc_console_target(ForegroundProbe)

# Steam manifest diagnostic probe — dumps the app id to install directory map
# that per-game profiles for Steam games are matched through.
add_executable(SteamAppProbe
    SteamAppProbe.cpp
)
target_link_libraries(SteamAppProbe PRIVATE sc_platform_win)
sc_target_defaults(SteamAppProbe)
sc_console_target(SteamAppProbe)

# Binding encoding checks — the packed and wire forms both have to keep
# decoding values written by earlier builds.
add_executable(BindingCodecProbe
    BindingCodecProbe.cpp
)
target_link_libraries(BindingCodecProbe PRIVATE sc_core)
sc_target_defaults(BindingCodecProbe)
sc_console_target(BindingCodecProbe)

# Virtual gamepad bus probe — dumps every bus the ViGEm client can see, which
# is the one question a "vigem_connect failed" report cannot answer on its own.
add_executable(ViGEmBusProbe
    ViGEmBusProbe.cpp
)
target_link_libraries(ViGEmBusProbe PRIVATE sc_platform_win ViGEmClient)
sc_target_defaults_relaxed(ViGEmBusProbe)
sc_console_target(ViGEmBusProbe)

# HapticTester — local-only interactive haptic tuning tool (source is gitignored).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/HapticTester.cpp")
    add_executable(HapticTester HapticTester.cpp)
    target_link_libraries(HapticTester PRIVATE sc_core sc_platform_win)
    if(MSVC)
        target_compile_options(HapticTester PRIVATE /W4 /utf-8)
    endif()
    sc_console_target(HapticTester)
endif()
