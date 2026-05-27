cmake_minimum_required(VERSION 3.20)
project(SteamlessController CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared HID + Steam Controller library code
add_library(SteamController STATIC
    src/hid/HidDevice.cpp
    src/steam/SteamController.cpp
)
target_include_directories(SteamController PUBLIC src)
target_link_libraries(SteamController PUBLIC hid setupapi)
if (MSVC)
    target_compile_options(SteamController PRIVATE /W4 /WX /utf-8 /wd4819 /wd4828)
endif()

# Diagnostic probe tool (console)
add_executable(SteamProbe
    src/main.cpp
)
target_link_libraries(SteamProbe PRIVATE SteamController)
if (MSVC)
    target_compile_options(SteamProbe PRIVATE /W4 /WX /utf-8 /wd4819 /wd4828)
    set_target_properties(SteamProbe PROPERTIES LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
endif()

# ViGEmClient (built from source, static)
add_library(ViGEmClient STATIC
    third_party/ViGEmClient-1.16.18.0/src/ViGEmClient.cpp
)
target_include_directories(ViGEmClient PUBLIC
    third_party/ViGEmClient-1.16.18.0/include
)
target_link_libraries(ViGEmClient PUBLIC setupapi)
if (MSVC)
    target_compile_options(ViGEmClient PRIVATE /W0 /utf-8 /wd4819 /wd4828)
endif()

# RawGameController diagnostic probe (WinRT)
add_executable(RawControllerProbe
    src/probe/RawControllerProbe.cpp
)
target_link_libraries(RawControllerProbe PRIVATE windowsapp)
if (MSVC)
    target_compile_options(RawControllerProbe PRIVATE /W4 /utf-8 /wd4819 /wd4828)
endif()

# System tray app (no console window)
add_executable(SteamlessController WIN32
    src/app/main.cpp
    src/app/TrayApp.cpp
    src/app/ControllerManager.cpp
    src/app/VirtualController.cpp
    src/app/TrackpadMouse.cpp
    resources/app.rc
)
target_include_directories(SteamlessController PRIVATE resources)
target_link_libraries(SteamlessController PRIVATE SteamController ViGEmClient)
if (MSVC)
    target_compile_options(SteamlessController PRIVATE /W4 /WX /utf-8 /wd4819 /wd4828)
endif()
