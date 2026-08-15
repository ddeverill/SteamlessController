#pragma once
#include "core/iface/IHidDevice.h"
#include <string>

// hidraw-backed IHidDevice. Report reads/writes/feature-reports map onto
// hidraw ioctls almost one-to-one; the interesting difference from Windows
// is that hidraw has no exclusivity concept at all (see Reopen() below) and
// that writes must use the report's exact declared length rather than being
// padded to a fixed maximum — hidraw passes write()'s byte count straight to
// the interrupt-OUT URB.
class LinuxHidDevice : public IHidDevice {
public:
    LinuxHidDevice() = default;
    ~LinuxHidDevice() override { Close(); }
    LinuxHidDevice(const LinuxHidDevice&) = delete;
    LinuxHidDevice& operator=(const LinuxHidDevice&) = delete;

    bool Open(const std::string& path) override;
    // No-op: hidraw devices accept concurrent opens from multiple processes,
    // each with its own read ring buffer — there is no share-mode concept to
    // change. Always succeeds once already open. See IDeviceReclaimer.h for
    // the larger story this enables (no Windows-style device-cycling helper
    // is needed on Linux at all).
    bool Reopen(HidShare share) override;
    void Close() override;
    bool IsOpen() const override { return m_fd >= 0; }

    bool SendOutputReport(const uint8_t* data, size_t size) override;
    bool WriteOutputReport(const uint8_t* data, size_t size) override;
    bool SendFeatureReport(const uint8_t* data, size_t size) override;

    uint32_t OutputReportByteLength()  const override { return m_outputReportLen; }
    uint32_t FeatureReportByteLength() const override { return m_featureReportLen; }

    size_t ReadInputReport(uint8_t* buffer, size_t size, uint32_t timeoutMs = 1000) override;

private:
    int      m_fd               = -1;
    uint32_t m_outputReportLen  = 64;
    uint32_t m_featureReportLen = 64;
};
