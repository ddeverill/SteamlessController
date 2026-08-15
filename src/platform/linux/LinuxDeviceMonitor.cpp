#include "LinuxDeviceMonitor.h"
#include <libudev.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

bool LinuxDeviceMonitor::Start(std::function<void()> onDeviceChange) {
    Stop();
    m_onDeviceChange = std::move(onDeviceChange);
    m_wakeFd = eventfd(0, EFD_NONBLOCK);
    if (m_wakeFd < 0) return false;

    m_running = true;
    m_thread  = std::thread(&LinuxDeviceMonitor::PollLoop, this);
    return true;
}

void LinuxDeviceMonitor::Stop() {
    m_running = false;
    if (m_wakeFd >= 0) {
        uint64_t one = 1;
        // Best-effort nudge; PollLoop's poll() also has a timeout so it
        // notices m_running going false even if this write is lost.
        ssize_t ignored = write(m_wakeFd, &one, sizeof(one));
        (void)ignored;
    }
    if (m_thread.joinable()) m_thread.join();
    if (m_wakeFd >= 0) { close(m_wakeFd); m_wakeFd = -1; }
    m_onDeviceChange = nullptr;
}

void LinuxDeviceMonitor::PollLoop() {
    struct udev* udev = udev_new();
    if (!udev) return;

    struct udev_monitor* mon = udev_monitor_new_from_netlink(udev, "udev");
    if (!mon) { udev_unref(udev); return; }
    udev_monitor_filter_add_match_subsystem_devtype(mon, "hidraw", nullptr);
    udev_monitor_enable_receiving(mon);
    const int monFd = udev_monitor_get_fd(mon);

    while (m_running) {
        struct pollfd fds[2] = {
            { monFd,   POLLIN, 0 },
            { m_wakeFd, POLLIN, 0 },
        };
        // A device add/remove is one event, not a burst — no debounce needed
        // the way ForegroundWatcher's window-focus churn does.
        if (poll(fds, 2, 1000) <= 0) continue;
        if (fds[1].revents & POLLIN) break;  // Stop() woke us
        if (!(fds[0].revents & POLLIN)) continue;

        struct udev_device* dev = udev_monitor_receive_device(mon);
        if (!dev) continue;
        udev_device_unref(dev);

        if (m_onDeviceChange) m_onDeviceChange();
    }

    udev_monitor_unref(mon);
    udev_unref(udev);
}
