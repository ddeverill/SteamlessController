#pragma once
#include "core/iface/IHidDevice.h"

// udev-backed hidraw enumeration. There is no udev/sysfs property exposing a
// HID top-level collection's usage page (unlike Windows' HidP_GetCaps), so
// matching usagePage/usage means opening each candidate node and parsing its
// raw report descriptor — see LinuxHidBackend.cpp for the byte-level walker.
// This is load-bearing for the Steam Controller specifically: its receiver
// exposes five hidraw nodes with identical VID/PID (four controller slots
// plus one dongle-management interface), and only the usage-page/usage
// filter tells them apart.
class LinuxHidBackend : public IHidBackend {
public:
    std::vector<HidDeviceInfo> Enumerate(uint16_t vid, uint16_t pid,
                                          uint16_t usagePage = 0, uint16_t usage = 0) override;
    std::unique_ptr<IHidDevice> Create() override;
};
