cmake_minimum_required(VERSION 3.20)
project(SteamlessController CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/ScDefaults.cmake)
enable_testing()

# Platform-neutral protocol/settings/orchestration logic. Builds identically
# on both platforms — see src/core/CMakeLists.txt.
add_subdirectory(src/core)

if(WIN32)
    add_subdirectory(third_party/ViGEmClient-b66d02d)
    add_subdirectory(src/platform/win)
    add_subdirectory(src/win)
    add_subdirectory(src/helper)
    add_subdirectory(src/probe)
elseif(UNIX AND NOT APPLE)
    add_subdirectory(src/platform/linux)
    add_subdirectory(src/ipc)
    add_subdirectory(src/daemon)
    add_subdirectory(src/cli)
else()
    message(FATAL_ERROR "SteamlessController supports Windows and Linux only")
endif()
