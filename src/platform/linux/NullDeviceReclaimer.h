#pragma once
#include "core/iface/IDeviceReclaimer.h"

// A permanent no-op, not a stub waiting to be filled in: hidraw has no
// exclusivity concept (confirmed on real hardware — a hidraw node accepts
// concurrent O_RDWR opens from multiple processes, each with its own read
// ring buffer), so the Windows problem this interface exists to solve
// ("forcibly take the device back from whoever has it locked exclusively")
// does not arise on Linux. See LinuxHidDevice::Reopen for the other half of
// this story.
class NullDeviceReclaimer : public IDeviceReclaimer {
public:
    Result Reclaim(const std::vector<std::string>&, std::string* report) override {
        if (report) *report = "device reclaim has no Linux equivalent (hidraw allows concurrent opens)";
        return Result::NotSupported;
    }
    bool Available() const override { return false; }
};
