#pragma once
#include <Windows.h>
#include <string>

namespace DeviceRestart {

// Restarts the PnP device node backing an HID interface path — equivalent to
// unplugging and replugging the device. Every process's open handles to it
// (including Steam's) are invalidated, and the re-arrival becomes an open
// race that whoever reacts to the arrival notification first wins.
//
// Requires administrator rights; fails with ERROR_ACCESS_DENIED in errorOut
// when the process is not elevated.
bool RestartInterfaceDevice(const std::wstring& interfacePath, DWORD* errorOut = nullptr);

}
