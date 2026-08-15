#pragma once
#include <functional>

// Notifies when a HID device is plugged/unplugged so ControllerManager can
// re-run its enumeration (ControllerManager::OnDeviceChange). Windows:
// WM_DEVICECHANGE. Linux: a udev monitor socket.
class IDeviceMonitor {
public:
    virtual ~IDeviceMonitor() = default;
    virtual bool Start(std::function<void()> onDeviceChange) = 0;
    virtual void Stop() = 0;
};
