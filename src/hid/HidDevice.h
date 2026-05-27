#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

class HidDevice {
public:
    // Returns device paths for all HID interfaces matching vid/pid/usagePage.
    // Pass usagePage=0 to return all matching interfaces.
    static std::vector<std::wstring> Enumerate(uint16_t vid, uint16_t pid, uint16_t usagePage = 0);

    HidDevice() = default;
    ~HidDevice() { Close(); }
    HidDevice(const HidDevice&) = delete;
    HidDevice& operator=(const HidDevice&) = delete;
    HidDevice(HidDevice&& o) noexcept;
    HidDevice& operator=(HidDevice&& o) noexcept;

    // Open with exclusive write access. Returns false if device is in use or not found.
    bool Open(const std::wstring& path);
    void Close();
    bool IsOpen() const { return m_handle != INVALID_HANDLE_VALUE; }

    // Send a HID output report (interrupt OUT / SET_REPORT Output type).
    // data[0] must be the report ID. Padded to OutputReportByteLength automatically.
    bool SendOutputReport(const uint8_t* data, size_t size);

    // Write a HID output report through the interrupt output endpoint.
    // Some devices use short output reports that should not be padded.
    bool WriteOutputReport(const uint8_t* data, size_t size);

    // Send a HID feature report (SET_REPORT Feature type via EP0 control pipe).
    // data[0] must be the feature report ID. Padded to FeatureReportByteLength automatically.
    // This is the command channel the original Steam Controller used for all firmware commands.
    bool SendFeatureReport(const uint8_t* data, size_t size);

    ULONG OutputReportByteLength()  const { return m_outputReportLen; }
    ULONG FeatureReportByteLength() const { return m_featureReportLen; }

    // Read the next HID input report. buffer[0] will be the report ID on return.
    // Returns bytes read, or 0 on timeout/error.
    size_t ReadInputReport(uint8_t* buffer, size_t size, uint32_t timeoutMs = 1000);

private:
    HANDLE m_handle           = INVALID_HANDLE_VALUE;
    HANDLE m_event            = INVALID_HANDLE_VALUE;
    ULONG  m_outputReportLen  = 64;
    ULONG  m_featureReportLen = 64;
};
