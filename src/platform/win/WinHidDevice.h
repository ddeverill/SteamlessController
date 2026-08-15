#pragma once
#include "core/iface/IHidDevice.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>

class WinHidDevice : public IHidDevice {
public:
    WinHidDevice() = default;
    ~WinHidDevice() override { Close(); }
    WinHidDevice(const WinHidDevice&) = delete;
    WinHidDevice& operator=(const WinHidDevice&) = delete;

    bool Open(const std::string& path) override;
    bool Reopen(HidShare share) override;
    void Close() override;
    bool IsOpen() const override { return m_handle != INVALID_HANDLE_VALUE; }

    bool SendOutputReport(const uint8_t* data, size_t size) override;
    bool WriteOutputReport(const uint8_t* data, size_t size) override;
    bool SendFeatureReport(const uint8_t* data, size_t size) override;

    uint32_t OutputReportByteLength()  const override { return m_outputReportLen; }
    uint32_t FeatureReportByteLength() const override { return m_featureReportLen; }

    size_t ReadInputReport(uint8_t* buffer, size_t size, uint32_t timeoutMs = 1000) override;

private:
    HANDLE       m_handle           = INVALID_HANDLE_VALUE;
    HANDLE       m_event            = INVALID_HANDLE_VALUE;
    uint32_t     m_outputReportLen  = 64;
    uint32_t     m_featureReportLen = 64;
    std::wstring m_path;
};

class WinHidBackend : public IHidBackend {
public:
    std::vector<HidDeviceInfo> Enumerate(uint16_t vid, uint16_t pid,
                                          uint16_t usagePage = 0, uint16_t usage = 0) override;
    std::unique_ptr<IHidDevice> Create() override;
};
