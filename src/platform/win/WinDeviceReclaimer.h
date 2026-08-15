#pragma once
#include "core/iface/IDeviceReclaimer.h"

// A thin, single-attempt wrapper over DeviceRestart::RestartInterfaceDevice.
// Not currently exercised: ControllerManager never calls IPlatform::Reclaimer()
// (device-node cycling on Windows is orchestrated by TrayApp directly, through
// DeviceRestart's richer CycleResult/elevated-helper machinery — see
// TrayApp::RestartControllerDevices — because that flow is asynchronous and
// stateful in a way this interface's single blocking call doesn't model). This
// class exists so WinPlatform has a real, working answer for the interface
// slot regardless.
class WinDeviceReclaimer : public IDeviceReclaimer {
public:
    Result Reclaim(const std::vector<std::string>& devicePaths, std::string* report) override;
    bool Available() const override;
};
