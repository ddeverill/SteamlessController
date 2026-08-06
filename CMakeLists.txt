cmake_minimum_required(VERSION 3.20)
project(SteamlessController CXX)

include(FetchContent)

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
    target_compile_options(SteamController PRIVATE /W4 /WX /utf-8)
endif()

# Diagnostic probe tool (console)
add_executable(SteamProbe
    src/main.cpp
)
target_link_libraries(SteamProbe PRIVATE SteamController)
if (MSVC)
    target_compile_options(SteamProbe PRIVATE /W4 /WX /utf-8)
    set_target_properties(SteamProbe PROPERTIES LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
endif()

# ViGEmClient (built from source, static)
add_library(ViGEmClient STATIC
    third_party/ViGEmClient-b66d02d/src/ViGEmClient.cpp
)
target_include_directories(ViGEmClient PUBLIC
    third_party/ViGEmClient-b66d02d/include
)
target_link_libraries(ViGEmClient PUBLIC setupapi)
if (MSVC)
    target_compile_options(ViGEmClient PRIVATE /W0 /utf-8 /wd4828)
endif()

# RawGameController diagnostic probe (WinRT)
add_executable(RawControllerProbe
    src/probe/RawControllerProbe.cpp
)
target_link_libraries(RawControllerProbe PRIVATE windowsapp)
if (MSVC)
    target_compile_options(RawControllerProbe PRIVATE /W4 /utf-8)
endif()

# Elevated on-demand helper — restarts the controller device node so the tray
# app (which never runs elevated) can take the device from a running Steam.
add_executable(SteamlessDeviceCycle WIN32
    src/helper/DeviceCycleHelper.cpp
    src/app/DeviceRestart.cpp
    src/app/HandleFinder.cpp
)
target_link_libraries(SteamlessDeviceCycle PRIVATE SteamController cfgmgr32)
if (MSVC)
    target_compile_options(SteamlessDeviceCycle PRIVATE /W4 /WX /utf-8)
endif()

# HapticTester — local-only interactive haptic tuning tool (source is gitignored)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/probe/HapticTester.cpp")
    add_executable(HapticTester src/probe/HapticTester.cpp)
    target_link_libraries(HapticTester PRIVATE SteamController)
    if (MSVC)
        target_compile_options(HapticTester PRIVATE /W4 /utf-8)
        set_target_properties(HapticTester PROPERTIES LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE")
    endif()
endif()

# WebView2 — downloaded from NuGet (.nupkg is a zip; FetchContent extracts it)
FetchContent_Declare(WebView2
    URL      https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2849.39
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(WebView2)

# Map CMake architecture to the NuGet lib subdirectory name
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WV2_ARCH "x64")
else()
    set(WV2_ARCH "x86")
endif()

# System tray app (no console window)
add_executable(SteamlessController WIN32
    src/app/main.cpp
    src/app/TrayApp.cpp
    src/app/ControllerManager.cpp
    src/app/VirtualController.cpp
    src/app/TrackpadMouse.cpp
    src/app/KeyInput.cpp
    src/app/SteamWatcher.cpp
    src/app/DeviceRestart.cpp
    src/app/EventLog.cpp
    src/app/HandleFinder.cpp
    src/app/RemapWindow.cpp
    resources/app.rc
)
target_include_directories(SteamlessController PRIVATE
    resources
    ${webview2_SOURCE_DIR}/build/native/include
)
target_link_libraries(SteamlessController PRIVATE
    SteamController
    ViGEmClient
    ${webview2_SOURCE_DIR}/build/native/${WV2_ARCH}/WebView2LoaderStatic.lib
    version
    ole32
    shlwapi
    cfgmgr32
)
if (MSVC)
    target_compile_options(SteamlessController PRIVATE /W4 /WX /utf-8 /wd4828)
endif()
