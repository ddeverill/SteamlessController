#pragma once
#include "core/iface/IDeviceMonitor.h"

// Not wired up: ControllerManager never calls IPlatform::Devices() (device-
// arrival on Windows reaches it via TrayApp's own WM_DEVICECHANGE handler
// calling ControllerManager::OnDeviceChange() directly, exactly as before
// the cross-platform port). This class exists so WinPlatform has a real
// interface slot to hand back; NotifyDeviceChange() is here for a future
// TrayApp that routes through IPlatform instead, and is simply never called
// today.
class WinDeviceMonitor : public IDeviceMonitor {
public:
    bool Start(std::function<void()> onDeviceChange) override {
        m_onDeviceChange = std::move(onDeviceChange);
        return true;
    }
    void Stop() override { m_onDeviceChange = nullptr; }

    void NotifyDeviceChange() { if (m_onDeviceChange) m_onDeviceChange(); }

private:
    std::function<void()> m_onDeviceChange;
};
