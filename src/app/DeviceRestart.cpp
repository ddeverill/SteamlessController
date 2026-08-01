#include "DeviceRestart.h"
#include <SetupAPI.h>

namespace DeviceRestart {

static bool SetDeviceState(HDEVINFO devs, SP_DEVINFO_DATA& devInfo, DWORD stateChange) {
    SP_PROPCHANGE_PARAMS pc{};
    pc.ClassInstallHeader.cbSize          = sizeof(SP_CLASSINSTALL_HEADER);
    pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    pc.StateChange                        = stateChange;
    pc.Scope                              = DICS_FLAG_GLOBAL;
    pc.HwProfile                          = 0;
    return SetupDiSetClassInstallParamsW(devs, &devInfo, &pc.ClassInstallHeader, sizeof(pc))
        && SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devs, &devInfo);
}

// Take the devnode down and bring it back, invalidating every open handle to
// it — including the one Steam holds, which is the whole point of a cycle.
//
// Disable/enable rather than the politer DICS_PROPCHANGE "restart", because
// on a Bluetooth HID child that restart reports success while leaving the
// device and Steam's handle completely untouched. CM_Query_And_Remove_SubTree
// is no better: it asks permission that an open handle can veto. A disable
// asks nobody, so it is the only one that reliably works on both transports.
//
// Windows may flag "reboot required" for deferred cleanup afterwards. The
// device still goes down and comes back, so that is not a failure — and
// unlike `pnputil /restart-device`, which refuses once that flag is set, a
// disable/enable keeps working for the rest of the session.
static bool CycleDevNode(HDEVINFO devs, SP_DEVINFO_DATA& devInfo, DWORD* errorOut) {
    if (!SetDeviceState(devs, devInfo, DICS_DISABLE)) {
        if (errorOut) *errorOut = GetLastError();
        // Nothing was taken down, so fall back to asking for a plain restart.
        return SetDeviceState(devs, devInfo, DICS_PROPCHANGE);
    }

    // Let the removal propagate before bringing the node back.
    Sleep(1000);

    // The enable must happen: a device left disabled needs Device Manager to
    // recover, which is a far worse failure than not cycling at all. Retry
    // once before giving up on it.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (SetDeviceState(devs, devInfo, DICS_ENABLE))
            return true;
        Sleep(500);
    }
    if (errorOut) *errorOut = GetLastError();
    return false;
}

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
            ok = CycleDevNode(devs, devInfo, errorOut);
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
