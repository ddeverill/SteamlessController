#include "DeviceRestart.h"
#include <SetupAPI.h>
#include <cfgmgr32.h>

namespace DeviceRestart {

static bool SetDeviceState(HDEVINFO devs, SP_DEVINFO_DATA& devInfo,
                           DWORD stateChange, DWORD scope) {
    SP_PROPCHANGE_PARAMS pc{};
    pc.ClassInstallHeader.cbSize          = sizeof(SP_CLASSINSTALL_HEADER);
    pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    pc.StateChange                        = stateChange;
    pc.Scope                              = scope;
    pc.HwProfile                          = 0;
    return SetupDiSetClassInstallParamsW(devs, &devInfo, &pc.ClassInstallHeader, sizeof(pc))
        && SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devs, &devInfo);
}

static bool IsDevNodeDisabled(DEVINST devInst) {
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, devInst, 0) != CR_SUCCESS)
        return false;  // can't tell — don't report a disabled state we didn't see
    return (status & DN_HAS_PROBLEM) != 0 && problem == CM_PROB_DISABLED;
}

// Take the devnode down and bring it back, invalidating every open handle to
// it — including the one Steam holds, which is the whole point of a cycle.
//
// Disable/enable rather than the politer DICS_PROPCHANGE "restart", because
// on a Bluetooth HID child that restart reports success while leaving the
// device and Steam's handle untouched. A disable asks nobody's permission,
// so it is the only thing that reliably breaks the handle.
//
// SetupDiCallClassInstaller's return value cannot be trusted here: a disable
// that Windows genuinely performed still came back as ERROR_INVALID_DATA on
// this Bluetooth HID child. So the devnode's own status is the source of
// truth, and the re-enable is never gated on what the disable claimed —
// getting that wrong strands the controller switched off, needing Device
// Manager to recover, which is far worse than failing to cycle at all.
static bool CycleDevNode(HDEVINFO devs, SP_DEVINFO_DATA& devInfo, DWORD* errorOut) {
    // Some devices reject a global scope change and only accept a
    // config-specific one, so try both before believing a disable was refused.
    if (!SetDeviceState(devs, devInfo, DICS_DISABLE, DICS_FLAG_GLOBAL))
        SetDeviceState(devs, devInfo, DICS_DISABLE, DICS_FLAG_CONFIGSPECIFIC);

    // Did it actually go down? This, not the return code above, decides
    // whether a real cycle happened.
    const bool wentDown = IsDevNodeDisabled(devInfo.DevInst);

    if (wentDown)
        Sleep(1000);  // let the removal propagate before bringing it back

    // Always re-enable, whatever the disable reported, and keep going until
    // the devnode confirms it is no longer disabled.
    bool enabled = !IsDevNodeDisabled(devInfo.DevInst);
    for (int attempt = 0; attempt < 3 && !enabled; ++attempt) {
        if (attempt) Sleep(500);
        SetDeviceState(devs, devInfo, DICS_ENABLE, DICS_FLAG_GLOBAL);
        if (IsDevNodeDisabled(devInfo.DevInst))
            SetDeviceState(devs, devInfo, DICS_ENABLE, DICS_FLAG_CONFIGSPECIFIC);
        enabled = !IsDevNodeDisabled(devInfo.DevInst);
    }

    if (!enabled) {
        // The controller is stranded disabled — the caller must surface this,
        // it is not a quiet failure.
        if (errorOut) *errorOut = GetLastError();
        return false;
    }

    if (!wentDown) {
        // Nothing was taken down, so no handle was broken. Fall back to
        // asking for a plain restart, which is enough on USB.
        if (errorOut) *errorOut = ERROR_NOT_SUPPORTED;
        return SetDeviceState(devs, devInfo, DICS_PROPCHANGE, DICS_FLAG_GLOBAL);
    }

    return true;
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
