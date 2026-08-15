#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// Whether a caller wants exclusive write access to a HID device or is content
// to share it. On Windows this maps to FILE_SHARE_* flags on a CreateFile
// reopen; on Linux hidraw has no exclusivity concept at all and every device
// answers Reopen() by simply returning true (see IDeviceReclaimer.h for the
// larger story of why "claim exclusive access" is a Windows-only problem).
enum class HidShare { ExclusiveWrite, Shared };

enum class HidBus { Unknown, Usb, Bluetooth };

// What HidBackend::Enumerate finds for one HID interface, and everything a
// caller needs to decide whether it is the interface it wants (vendor
// collection vs. mouse/keyboard emulation collection) and how it is
// connected (see SteamController::TransportFrom).
struct HidDeviceInfo {
    std::string path;    // "\\?\hid#..." on Windows, "/dev/hidraw3" on Linux
    uint16_t    vid = 0;
    uint16_t    pid = 0;
    HidBus      bus = HidBus::Unknown;
    std::string serial;
};

// One open HID device: raw report I/O only, no enumeration (that is
// IHidBackend::Enumerate, which does not require a device to be open).
class IHidDevice {
public:
    virtual ~IHidDevice() = default;

    virtual bool Open(const std::string& path) = 0;
    // Reopen with a different share mode. Windows: closes and reopens the
    // underlying handle with new FILE_SHARE_* flags. Linux: no-op, always
    // returns true — hidraw devices are always shareable.
    virtual bool Reopen(HidShare share) = 0;
    virtual void Close() = 0;
    virtual bool IsOpen() const = 0;

    // Send a HID output report (interrupt OUT / SET_REPORT Output type).
    // data[0] must be the report ID.
    virtual bool SendOutputReport(const uint8_t* data, size_t size) = 0;
    // Write an output report through the interrupt OUT endpoint.
    virtual bool WriteOutputReport(const uint8_t* data, size_t size) = 0;
    // Send a HID feature report (SET_REPORT Feature type via the control pipe).
    // data[0] must be the feature report ID.
    virtual bool SendFeatureReport(const uint8_t* data, size_t size) = 0;

    virtual uint32_t OutputReportByteLength()  const = 0;
    virtual uint32_t FeatureReportByteLength() const = 0;

    // Read the next HID input report. buffer[0] is the report ID on return.
    // Returns bytes read, or 0 on timeout/error.
    virtual size_t ReadInputReport(uint8_t* buffer, size_t size, uint32_t timeoutMs = 1000) = 0;
};

// Enumeration + construction. usagePage/usage filter to a specific top-level
// HID collection (0 matches any) — on Windows this reads HidP_GetCaps on
// preparsed data; on Linux it means parsing the raw report descriptor, since
// there is no udev/sysfs property that exposes HID usage page.
class IHidBackend {
public:
    virtual ~IHidBackend() = default;

    virtual std::vector<HidDeviceInfo> Enumerate(uint16_t vid, uint16_t pid,
                                                  uint16_t usagePage = 0,
                                                  uint16_t usage = 0) = 0;
    virtual std::unique_ptr<IHidDevice> Create() = 0;
};
