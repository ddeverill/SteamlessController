#include "DeviceRestart.h"
#include <SetupAPI.h>

namespace DeviceRestart {

bool RestartInterfaceDevice(const std::wstring& interfacePath, DWORD* errorOut) {
    if (errorOut) *errorOut = ERROR_SUCCESS;

    HDEVINFO devs = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (devs == INVALID_HANDLE_VALUE) {
        if (errorOut) *errorOut = GetLastError();
        return false;
    }

    bool ok = false;
    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);
    if (SetupDiOpenDeviceInterfaceW(devs, interfacePath.c_str(), 0, &ifData)) {
        // Query with no output buffer purely to resolve the owning devnode —
        // the call fails with ERROR_INSUFFICIENT_BUFFER but still fills devInfo.
        SP_DEVINFO_DATA devInfo{};
        devInfo.cbSize = sizeof(devInfo);
        SetupDiGetDeviceInterfaceDetailW(devs, &ifData, nullptr, 0, nullptr, &devInfo);

        if (devInfo.DevInst != 0) {
            SP_PROPCHANGE_PARAMS pc{};
            pc.ClassInstallHeader.cbSize          = sizeof(SP_CLASSINSTALL_HEADER);
            pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
            pc.StateChange                        = DICS_PROPCHANGE;  // restart
            pc.Scope                              = DICS_FLAG_GLOBAL;
            pc.HwProfile                          = 0;

            if (SetupDiSetClassInstallParamsW(devs, &devInfo,
                                              &pc.ClassInstallHeader, sizeof(pc))
                && SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devs, &devInfo)) {
                ok = true;
            } else if (errorOut) {
                *errorOut = GetLastError();
            }
        } else if (errorOut) {
            *errorOut = GetLastError();
        }
    } else if (errorOut) {
        *errorOut = GetLastError();
    }

    SetupDiDestroyDeviceInfoList(devs);
    return ok;
}

}
