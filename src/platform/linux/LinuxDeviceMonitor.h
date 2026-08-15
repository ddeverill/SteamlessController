#pragma once
#include "core/iface/IDeviceMonitor.h"
#include <atomic>
#include <thread>

// Watches udev for hidraw add/remove events, the Linux analog of Windows'
// WM_DEVICECHANGE. Runs its own thread with a netlink socket rather than
// requiring the daemon's main loop to poll udev directly, keeping the
// dependency on libudev contained to this one file.
class LinuxDeviceMonitor : public IDeviceMonitor {
public:
    ~LinuxDeviceMonitor() override { Stop(); }

    bool Start(std::function<void()> onDeviceChange) override;
    void Stop() override;

private:
    void PollLoop();

    std::function<void()> m_onDeviceChange;
    std::thread           m_thread;
    std::atomic<bool>     m_running{false};
    int                   m_wakeFd = -1;  // eventfd, closes PollLoop's netlink poll on Stop()
};
