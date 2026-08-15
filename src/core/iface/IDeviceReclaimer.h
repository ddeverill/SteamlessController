#pragma once
#include <string>
#include <vector>

// Forcibly reclaiming a HID device from whoever else has it open exclusively.
// This is a Windows-only problem: Windows device nodes support exclusive
// FILE_SHARE flags, so Steam (or anything else) can lock this app out of the
// controller entirely, and DeviceRestart/DeviceCycleHelper cycle the devnode
// to break that lock. Linux hidraw has no exclusivity concept at all — every
// opener gets its own read ring buffer — so NullDeviceReclaimer is a
// permanent, correct no-op there, not a stub waiting to be filled in.
class IDeviceReclaimer {
public:
    enum class Result { Succeeded, Failed, NotSupported };

    virtual ~IDeviceReclaimer() = default;

    virtual Result Reclaim(const std::vector<std::string>& devicePaths, std::string* report) = 0;
    virtual bool Available() const = 0;
};
