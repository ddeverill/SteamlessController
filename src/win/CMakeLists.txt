# WebView2 — downloaded from NuGet (.nupkg is a zip; FetchContent extracts
# it). Confined to this directory so a Linux configure never touches it.
include(FetchContent)
FetchContent_Declare(WebView2
    URL      https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.2849.39
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(WebView2)

# Map CMake architecture to the NuGet lib subdirectory name.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WV2_ARCH "x64")
else()
    set(WV2_ARCH "x86")
endif()

# System tray app (no console window).
add_executable(SteamlessController WIN32
    main.cpp
    TrayApp.h
    TrayApp.cpp
    RemapWindow.h
    RemapWindow.cpp
    ${CMAKE_SOURCE_DIR}/resources/app.rc
)
target_include_directories(SteamlessController PRIVATE
    ${CMAKE_SOURCE_DIR}/resources
    ${webview2_SOURCE_DIR}/build/native/include
)
target_link_libraries(SteamlessController PRIVATE
    sc_core
    sc_platform_win
    ViGEmClient
    ${webview2_SOURCE_DIR}/build/native/${WV2_ARCH}/WebView2LoaderStatic.lib
    version
    ole32
    shell32
    shlwapi
    cfgmgr32
    windowsapp
)
# WebView2's generated headers trip the same BOM warning as ViGEmClient's.
sc_target_defaults_relaxed(SteamlessController)
