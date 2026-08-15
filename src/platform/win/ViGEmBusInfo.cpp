#include "ViGEmBusInfo.h"

#include <windows.h>
#include <setupapi.h>

// GUID_DEVINTERFACE_BUSENUM_VIGEM. Taken from the vendored client rather than
// copied, so updating third_party cannot leave a stale value behind here — the
// header itself tells forks to change the GUID, which is the whole reason this
// file exists. Declared here and defined in ViGEmClient.obj, which includes
// <initguid.h> ahead of the same header.
#include <ViGEm/km/BusShared.h>

namespace {

// SPDRP_FRIENDLYNAME is what Device Manager shows and what a fork rebrands:
// "Nefarius Virtual Gamepad Emulation Bus" against "Oculus Virtual Gamepad
// Emulation Bus". Not every devnode sets one, so fall back to its description.
std::wstring ReadName(HDEVINFO set, SP_DEVINFO_DATA& info) {
    for (const DWORD prop : {SPDRP_FRIENDLYNAME, SPDRP_DEVICEDESC}) {
        DWORD bytes = 0;
        SetupDiGetDeviceRegistryPropertyW(set, &info, prop, nullptr, nullptr, 0, &bytes);
        if (bytes == 0) continue;

        std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, prop, nullptr,
                                               reinterpret_cast<PBYTE>(value.data()),
                                               bytes, nullptr))
            continue;

        value.resize(wcslen(value.c_str()));
        if (!value.empty()) return value;
    }
    return {};
}

}  // namespace

std::vector<ViGEmBusInfo::Bus> ViGEmBusInfo::Enumerate() {
    std::vector<Bus> buses;

    const HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_BUSENUM_VIGEM,
                                              nullptr, nullptr,
                                              DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return buses;

    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof(iface);
    for (DWORD i = 0;
         SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_BUSENUM_VIGEM,
                                     i, &iface);
         ++i) {
        DWORD bytes = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &bytes, nullptr);
        if (bytes == 0) continue;

        std::vector<BYTE> buffer(bytes);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA info{};
        info.cbSize = sizeof(info);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, bytes, nullptr, &info))
            continue;

        buses.push_back({detail->DevicePath, ReadName(set, info)});
    }

    SetupDiDestroyDeviceInfoList(set);
    return buses;
}

std::wstring ViGEmBusInfo::Describe(const std::vector<Bus>& buses) {
    if (buses.empty())
        return L"no virtual gamepad bus is present — ViGEmBus is not installed, "
               L"or its device is disabled in Device Manager under System devices";

    // The count leads, because more than one is the answer to the question a
    // maintainer reading this line is actually asking.
    std::wstring text = std::to_wstring(buses.size());
    text += (buses.size() == 1)
          ? L" virtual gamepad bus present: "
          : L" virtual gamepad buses present — the client connects to whichever "
            L"answers first, which may not be the one this app needs: ";

    for (size_t i = 0; i < buses.size(); ++i) {
        if (i != 0) text += L", ";
        text += buses[i].friendlyName.empty() ? buses[i].devicePath
                                              : buses[i].friendlyName;
    }
    return text;
}
