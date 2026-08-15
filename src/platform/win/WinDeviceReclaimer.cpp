#include "WinDeviceReclaimer.h"
#include "DeviceRestart.h"
#include "core/Text.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

IDeviceReclaimer::Result WinDeviceReclaimer::Reclaim(const std::vector<std::string>& devicePaths,
                                                      std::string* report) {
    bool anyRestarted = false;
    DeviceRestart::CycleResult summary;
    for (const auto& path : devicePaths) {
        DeviceRestart::CycleResult r;
        if (DeviceRestart::RestartInterfaceDevice(Utf8ToWide(path), r))
            anyRestarted = true;
        const bool better = r.kind > summary.kind
            || (r.kind == summary.kind
                && summary.vetoType == PNP_VetoTypeUnknown
                && r.vetoType != PNP_VetoTypeUnknown);
        if (better) summary = r;
    }
    if (report)
        *report = std::string("cycle result: ") + DeviceRestart::CycleKindName(summary.kind);
    // RestartInterfaceDevice requires elevation; failing with ERROR_ACCESS_DENIED
    // when not elevated is a Failed result here, not NotSupported — the
    // mechanism exists, this process just isn't privileged enough right now.
    return anyRestarted ? Result::Succeeded : Result::Failed;
}

bool WinDeviceReclaimer::Available() const {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev{};
    DWORD size = sizeof(elev);
    const bool ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size) != 0;
    CloseHandle(token);
    return ok && elev.TokenIsElevated != 0;
}
