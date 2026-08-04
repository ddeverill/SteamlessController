#pragma once
#include <Windows.h>
#include <string>
#include <vector>

namespace DeviceRestart {

struct RestartOutcome {
    std::wstring path;
    bool  cycled           = false;  // devnode genuinely went down and came back
    bool  restartRequested = false;  // disable refused; asked for a plain restart
    bool  enabled          = true;   // false means it was left disabled — surface it
    DWORD error            = ERROR_SUCCESS;
};

// Restarts the PnP device nodes backing the given HID interface paths —
// equivalent to unplugging and replugging them. Every process's open handles
// (including Steam's) are invalidated, and the re-arrival becomes an open race
// that whoever reacts to the arrival notification first wins.
//
// Batched deliberately: all the devices are disabled, then one settle covers
// the whole set, then all are re-enabled. The settle dominates the cost, so
// cycling a four-slot receiver one device at a time paid it four times over —
// measured at 6.6s before a reclaim could even begin.
//
// Requires administrator rights; fails with ERROR_ACCESS_DENIED otherwise.
std::vector<RestartOutcome> RestartInterfaceDevices(const std::vector<std::wstring>& paths);

// Single-device convenience wrapper.
bool RestartInterfaceDevice(const std::wstring& interfacePath, DWORD* errorOut = nullptr);

}
