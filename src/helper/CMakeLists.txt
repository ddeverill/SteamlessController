# Elevated on-demand helper — restarts the controller device node so the tray
# app (which never runs elevated) can take the device from a running Steam.
add_executable(SteamlessDeviceCycle WIN32
    DeviceCycleHelper.cpp
)
target_link_libraries(SteamlessDeviceCycle PRIVATE sc_core sc_platform_win cfgmgr32)
sc_target_defaults(SteamlessDeviceCycle)
