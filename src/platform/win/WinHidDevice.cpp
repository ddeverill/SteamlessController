#include "WinHidDevice.h"
#include "core/Text.h"
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

// Bluetooth (BLE HID-over-GATT, or classic Bluetooth HID) paths carry
// recognisable substrings; everything else enumerated through the HID class
// GUID is USB. This is Windows-path-specific and has no Linux counterpart —
// LinuxHidBackend reads bus type straight from HIDIOCGRAWINFO instead.
HidBus BusFromPath(const std::wstring& path) {
    std::wstring p = path;
    for (auto& c : p) c = static_cast<wchar_t>(towlower(c));
    if (p.find(L"{00001812-0000-1000-8000-00805f9b34fb}") != std::wstring::npos ||
        p.find(L"bthledevice") != std::wstring::npos ||
        p.find(L"bthenum") != std::wstring::npos)
        return HidBus::Bluetooth;
    return HidBus::Usb;
}

}  // namespace

std::unique_ptr<IHidDevice> WinHidBackend::Create() {
    return std::make_unique<WinHidDevice>();
}

std::vector<HidDeviceInfo> WinHidBackend::Enumerate(uint16_t vid, uint16_t pid,
                                                     uint16_t usagePage, uint16_t usage) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE)
        return {};

    std::vector<HidDeviceInfo> result;
    SP_DEVICE_INTERFACE_DATA ifaceData{};
    ifaceData.cbSize = sizeof(ifaceData);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &ifaceData); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, nullptr, 0, &needed, nullptr);
        if (needed == 0)
            continue;

        auto detailBuf = std::make_unique<uint8_t[]>(needed);
        auto* detail   = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuf.get());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, detail, needed, nullptr, nullptr))
            continue;

        // Open with no access just to query attributes — avoids needing exclusive open.
        HANDLE h = CreateFileW(detail->DevicePath, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        HIDD_ATTRIBUTES attrs{};
        attrs.Size = sizeof(attrs);
        bool match = HidD_GetAttributes(h, &attrs)
                  && attrs.VendorID == vid
                  && (pid == 0 || attrs.ProductID == pid);

        if (match && usagePage != 0) {
            PHIDP_PREPARSED_DATA preparsed;
            if (HidD_GetPreparsedData(h, &preparsed)) {
                HIDP_CAPS caps{};
                if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS)
                    match = caps.UsagePage == usagePage
                         && (usage == 0 || caps.Usage == usage);
                HidD_FreePreparsedData(preparsed);
            } else {
                match = false;
            }
        }

        if (match) {
            HidDeviceInfo info;
            info.path = WideToUtf8(detail->DevicePath);
            info.vid  = attrs.VendorID;
            info.pid  = attrs.ProductID;
            info.bus  = BusFromPath(detail->DevicePath);
            wchar_t serial[128] = {};
            if (HidD_GetSerialNumberString(h, serial, sizeof(serial)))
                info.serial = WideToUtf8(serial);
            result.push_back(std::move(info));
        }

        CloseHandle(h);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

// ---------------------------------------------------------------------------
// Open / Close
// ---------------------------------------------------------------------------

bool WinHidDevice::Open(const std::string& path) {
    Close();
    const std::wstring widePath = Utf8ToWide(path);

    // Shared open for idle tracking — Steam can coexist while game mode is off.
    // Reopen(HidShare::ExclusiveWrite) prefers FILE_SHARE_READ when game mode
    // activates, blocking Steam from obtaining write access when the OS permits it.
    m_handle = CreateFileW(widePath.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED,
                           nullptr);

    if (m_handle == INVALID_HANDLE_VALUE) {
        wprintf(L"CreateFileW failed for %s: error %lu\n", widePath.c_str(), GetLastError());
        return false;
    }

    m_path  = widePath;
    m_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_event == INVALID_HANDLE_VALUE) {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        return false;
    }

    // Query report sizes. HidD_SetOutputReport and HidD_SetFeature each require
    // their buffer to be exactly the maximum report length of their respective type.
    PHIDP_PREPARSED_DATA preparsed;
    if (HidD_GetPreparsedData(m_handle, &preparsed)) {
        HIDP_CAPS caps{};
        if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
            if (caps.OutputReportByteLength  > 0) m_outputReportLen  = caps.OutputReportByteLength;
            if (caps.FeatureReportByteLength > 0) m_featureReportLen = caps.FeatureReportByteLength;
        }
        HidD_FreePreparsedData(preparsed);
    }
    return true;
}

void WinHidDevice::Close() {
    if (m_handle != INVALID_HANDLE_VALUE) {
        CancelIo(m_handle);
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
    if (m_event != INVALID_HANDLE_VALUE) {
        CloseHandle(m_event);
        m_event = INVALID_HANDLE_VALUE;
    }
    m_path.clear();
}

bool WinHidDevice::Reopen(HidShare share) {
    if (m_path.empty()) return false;
    std::wstring savedPath = m_path;
    const DWORD shareMode = share == HidShare::ExclusiveWrite
        ? FILE_SHARE_READ
        : (FILE_SHARE_READ | FILE_SHARE_WRITE);

    // Cancel pending I/O before closing so the read thread's overlapped op
    // drains cleanly. The caller must ensure the read thread is not running.
    if (m_handle != INVALID_HANDLE_VALUE) {
        CancelIo(m_handle);
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }

    m_handle = CreateFileW(savedPath.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           shareMode,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED,
                           nullptr);
    if (m_handle == INVALID_HANDLE_VALUE) {
        wprintf(L"Reopen failed for %s (shareMode=0x%lX): error %lu\n",
                savedPath.c_str(), shareMode, GetLastError());
        return false;
    }

    m_path = savedPath;
    return true;
}

// ---------------------------------------------------------------------------
// I/O
// ---------------------------------------------------------------------------

bool WinHidDevice::SendOutputReport(const uint8_t* data, size_t size) {
    // HidD_SetOutputReport requires the buffer to be exactly OutputReportByteLength.
    // Pad with zeros if the caller provided a shorter buffer (e.g. a 7-byte report
    // when the device's max output report is 64 bytes).
    if (size < m_outputReportLen) {
        std::vector<uint8_t> padded(m_outputReportLen, 0);
        std::memcpy(padded.data(), data, size);
        BOOLEAN ok = HidD_SetOutputReport(m_handle, padded.data(), m_outputReportLen);
        DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok)
            printf("SendOutputReport(0x%02X) failed: error %lu\n", data[0], err);
        return ok == TRUE;
    }

    BOOLEAN ok = HidD_SetOutputReport(m_handle,
                                       const_cast<PVOID>(static_cast<const void*>(data)),
                                       static_cast<ULONG>(size));
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok)
        printf("SendOutputReport(0x%02X) failed: error %lu\n", data[0], err);
    return ok == TRUE;
}

bool WinHidDevice::WriteOutputReport(const uint8_t* data, size_t size) {
    auto writeOnce = [&](const uint8_t* buffer, size_t len) -> bool {
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event)
            return false;

        OVERLAPPED ov{};
        ov.hEvent = event;

        DWORD bytesWritten = 0;
        BOOL ok = WriteFile(m_handle, buffer, static_cast<DWORD>(len), &bytesWritten, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            DWORD wait = WaitForSingleObject(event, 1000);
            if (wait == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(m_handle, &ov, &bytesWritten, FALSE);
            } else {
                // CancelIo is asynchronous — the kernel can still complete the IRP
                // and write into `ov` after it returns. Block until the cancelled
                // (or already-completed) I/O drains before `ov` leaves scope.
                CancelIo(m_handle);
                ok = GetOverlappedResult(m_handle, &ov, &bytesWritten, TRUE);
            }
        }

        DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        CloseHandle(event);

        if (!ok)
            printf("WriteOutputReport(0x%02X, %zu bytes) failed: error %lu\n", data[0], len, err);
        return ok == TRUE;
    };

    if (size < m_outputReportLen) {
        std::vector<uint8_t> padded(m_outputReportLen, 0);
        std::memcpy(padded.data(), data, size);
        return writeOnce(padded.data(), padded.size());
    }

    return writeOnce(data, size);
}

bool WinHidDevice::SendFeatureReport(const uint8_t* data, size_t size) {
    std::vector<uint8_t> buf(m_featureReportLen, 0);
    size_t copyLen = size < m_featureReportLen ? size : m_featureReportLen;
    std::memcpy(buf.data(), data, copyLen);
    BOOLEAN ok = HidD_SetFeature(m_handle, buf.data(), m_featureReportLen);
    if (!ok)
        printf("SendFeatureReport(reportId=0x%02X cmd=0x%02X) failed: error %lu\n",
               buf[0], buf[1], GetLastError());
    return ok == TRUE;
}

size_t WinHidDevice::ReadInputReport(uint8_t* buffer, size_t size, uint32_t timeoutMs) {
    OVERLAPPED ov{};
    ov.hEvent = m_event;
    ResetEvent(m_event);

    DWORD bytesRead = 0;
    if (!ReadFile(m_handle, buffer, static_cast<DWORD>(size), &bytesRead, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING)
            return 0;

        DWORD wait = WaitForSingleObject(m_event, timeoutMs);
        if (wait != WAIT_OBJECT_0) {
            // CancelIo is asynchronous — drain before ov leaves scope.
            CancelIo(m_handle);
            GetOverlappedResult(m_handle, &ov, &bytesRead, TRUE);
            return 0;
        }
        if (!GetOverlappedResult(m_handle, &ov, &bytesRead, FALSE))
            return 0;
    }

    return static_cast<size_t>(bytesRead);
}
