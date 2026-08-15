#include "LinuxHidBackend.h"
#include "LinuxHidDevice.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <libudev.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <memory>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

// Walks a HID report descriptor's short items looking for a top-level
// (depth 0) Application collection tagged with the given usage page/usage.
// Only short items appear in any report descriptor this app has to deal
// with (the Steam Controller's included — confirmed by inspection), so long
// items are not handled.
//
// There is no udev/sysfs property that exposes this — HID_ID only carries
// bus/vendor/product — so this is the one way to tell the Steam Controller's
// four controller-slot hidraw nodes apart from its fifth, dongle-management
// node, all of which share one VID/PID.
bool HasTopLevelCollection(const uint8_t* desc, int len, uint16_t wantPage, uint16_t wantUsage) {
    static constexpr int kSizes[4] = { 0, 1, 2, 4 };

    int      depth        = 0;
    uint16_t usagePage    = 0;   // Global item — persists until changed
    long     pendingUsage = -1;  // Local item — cleared after every Main item

    int i = 0;
    while (i < len) {
        const uint8_t prefix = desc[i++];
        const int     size   = kSizes[prefix & 0x03];
        const int     type   = (prefix >> 2) & 0x03;  // 0=Main 1=Global 2=Local
        const int     tag    = (prefix >> 4) & 0x0F;
        if (i + size > len) break;

        uint32_t value = 0;
        for (int k = 0; k < size; ++k) value |= static_cast<uint32_t>(desc[i + k]) << (8 * k);
        i += size;

        if (type == 1 && tag == 0x0) {              // Usage Page
            usagePage = static_cast<uint16_t>(value);
        } else if (type == 2 && tag == 0x0) {        // Usage
            pendingUsage = static_cast<long>(value);
        } else if (type == 0 && tag == 0xA) {        // Collection
            if (depth == 0 && value == 0x01           // Application collection
                    && usagePage == wantPage && pendingUsage == static_cast<long>(wantUsage))
                return true;
            ++depth;
            pendingUsage = -1;
        } else if (type == 0 && tag == 0xC) {        // End Collection
            --depth;
            pendingUsage = -1;
        } else if (type == 0) {                      // any other Main item
            pendingUsage = -1;
        }
    }
    return false;
}

}  // namespace

std::unique_ptr<IHidDevice> LinuxHidBackend::Create() {
    return std::make_unique<LinuxHidDevice>();
}

std::vector<HidDeviceInfo> LinuxHidBackend::Enumerate(uint16_t vid, uint16_t pid,
                                                       uint16_t usagePage, uint16_t usage) {
    std::vector<HidDeviceInfo> result;

    struct udev* udev = udev_new();
    if (!udev) return result;

    struct udev_enumerate* en = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(en, "hidraw");
    udev_enumerate_scan_devices(en);

    struct udev_list_entry* entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
        const char* syspath = udev_list_entry_get_name(entry);
        struct udev_device* dev = udev_device_new_from_syspath(udev, syspath);
        if (!dev) continue;

        const char* devnode = udev_device_get_devnode(dev);
        if (!devnode) { udev_device_unref(dev); continue; }

        // Read-only is enough to query attributes and the report descriptor;
        // the caller opens its own read/write handle via Create()->Open().
        const int fd = open(devnode, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { udev_device_unref(dev); continue; }

        struct hidraw_devinfo info{};
        bool match = ioctl(fd, HIDIOCGRAWINFO, &info) == 0
                  && static_cast<uint16_t>(info.vendor)  == vid
                  && (pid == 0 || static_cast<uint16_t>(info.product) == pid);

        if (match && usagePage != 0) {
            int size = 0;
            if (ioctl(fd, HIDIOCGRDESCSIZE, &size) < 0 || size <= 0) {
                match = false;
            } else {
                struct hidraw_report_descriptor rdesc{};
                rdesc.size = static_cast<uint32_t>(size);
                if (ioctl(fd, HIDIOCGRDESC, &rdesc) < 0)
                    match = false;
                else
                    match = HasTopLevelCollection(rdesc.value, size, usagePage, usage);
            }
        }

        if (match) {
            HidDeviceInfo hi;
            hi.path = devnode;
            hi.vid  = static_cast<uint16_t>(info.vendor);
            hi.pid  = static_cast<uint16_t>(info.product);
            hi.bus  = info.bustype == BUS_BLUETOOTH ? HidBus::Bluetooth : HidBus::Usb;

            char uniq[64] = {};
            if (ioctl(fd, HIDIOCGRAWUNIQ(sizeof(uniq)), uniq) >= 0)
                hi.serial = uniq;

            result.push_back(std::move(hi));
        }

        close(fd);
        udev_device_unref(dev);
    }

    udev_enumerate_unref(en);
    udev_unref(udev);
    return result;
}
