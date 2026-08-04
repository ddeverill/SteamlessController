#include "DeviceRestart.h"
#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <algorithm>

namespace DeviceRestart {

namespace {

// How long the whole batch is left down before being brought back. One second
// was a conservative guess; the settle only has to outlast the removal
// propagating, and paying it once per batch rather than once per device is
// what makes a multi-slot receiver usable.
constexpr DWORD kSettleMs = 400;

bool SetDeviceState(HDEVINFO devs, SP_DEVINFO_DATA& devInfo,
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

bool IsDevNodeDisabled(DEVINST devInst) {
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, devInst, 0) != CR_SUCCESS)
        return false;  // can't tell — don't report a disabled state we didn't see
    return (status & DN_HAS_PROBLEM) != 0 && problem == CM_PROB_DISABLED;
}

struct Target {
    std::wstring    path;
    SP_DEVINFO_DATA devInfo{};
    bool            valid    = false;
    bool            wentDown = false;
    DWORD           error    = ERROR_SUCCESS;
};

}  // namespace

// Disable/enable rather than the politer DICS_PROPCHANGE "restart", because on
// a Bluetooth HID child that restart reports success while leaving the device
// and the holder's handle untouched. CM_Query_And_Remove_SubTree is no better:
// it asks permission that an open handle can veto. A disable asks nobody.
//
// SetupDiCallClassInstaller's return value cannot be trusted here — a disable
// Windows genuinely performed still came back ERROR_INVALID_DATA — so the
// devnode's own status is the source of truth, and the re-enable never depends
// on what the disable claimed. Getting that wrong strands the controller
// switched off, needing Device Manager to recover, which is far worse than
// failing to cycle at all.
std::vector<RestartOutcome> RestartInterfaceDevices(const std::vector<std::wstring>& paths) {
    std::vector<RestartOutcome> results;
    results.reserve(paths.size());
    if (paths.empty()) return results;

    HDEVINFO devs = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (devs == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        for (auto const& path : paths)
            results.push_back({path, false, false, true, err});
        return results;
    }

    std::vector<Target> targets;
    targets.reserve(paths.size());
    for (auto const& path : paths) {
        Target target;
        target.path = path;
        SP_DEVICE_INTERFACE_DATA ifData{};
        ifData.cbSize = sizeof(ifData);
        if (SetupDiOpenDeviceInterfaceW(devs, path.c_str(), 0, &ifData)) {
            // Query with no output buffer purely to resolve the owning devnode
            // — the call fails with ERROR_INSUFFICIENT_BUFFER but still fills
            // devInfo.
            target.devInfo.cbSize = sizeof(target.devInfo);
            SetupDiGetDeviceInterfaceDetailW(devs, &ifData, nullptr, 0, nullptr, &target.devInfo);
            target.valid = target.devInfo.DevInst != 0;
        }
        if (!target.valid) target.error = GetLastError();
        targets.push_back(std::move(target));
    }

    // Take the whole set down first, so one settle covers all of them.
    for (auto& target : targets) {
        if (!target.valid) continue;
        // Some devices reject a global scope change and accept only a
        // config-specific one, so try both before believing a disable failed.
        if (!SetDeviceState(devs, target.devInfo, DICS_DISABLE, DICS_FLAG_GLOBAL))
            SetDeviceState(devs, target.devInfo, DICS_DISABLE, DICS_FLAG_CONFIGSPECIFIC);
        target.wentDown = IsDevNodeDisabled(target.devInfo.DevInst);
    }

    if (std::any_of(targets.begin(), targets.end(),
                    [](const Target& t) { return t.wentDown; }))
        Sleep(kSettleMs);

    for (auto& target : targets) {
        if (!target.valid) {
            results.push_back({target.path, false, false, true, target.error});
            continue;
        }

        // The re-enable always runs, and keeps going until the devnode
        // confirms it is no longer disabled.
        bool enabled = !IsDevNodeDisabled(target.devInfo.DevInst);
        for (int attempt = 0; attempt < 3 && !enabled; ++attempt) {
            if (attempt) Sleep(250);
            SetDeviceState(devs, target.devInfo, DICS_ENABLE, DICS_FLAG_GLOBAL);
            if (IsDevNodeDisabled(target.devInfo.DevInst))
                SetDeviceState(devs, target.devInfo, DICS_ENABLE, DICS_FLAG_CONFIGSPECIFIC);
            enabled = !IsDevNodeDisabled(target.devInfo.DevInst);
        }

        RestartOutcome outcome{target.path, target.wentDown, false, enabled, ERROR_SUCCESS};
        if (!enabled) {
            outcome.error = GetLastError();
        } else if (!target.wentDown) {
            // Nothing came down, so no handle was broken. Fall back to asking
            // for a plain restart, which is enough on transports where the
            // class installer honours it.
            outcome.restartRequested =
                SetDeviceState(devs, target.devInfo, DICS_PROPCHANGE, DICS_FLAG_GLOBAL);
            outcome.error = ERROR_NOT_SUPPORTED;
        }
        results.push_back(std::move(outcome));
    }

    SetupDiDestroyDeviceInfoList(devs);
    return results;
}

namespace {

std::wstring CycleTargetsPath() {
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return {};
    const std::wstring dir = std::wstring(local) + L"\\SteamlessController";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\cycle-targets.txt";
}

// A handover left behind by a crashed run must not steer a later cycle at
// devices nobody asked about.
constexpr ULONGLONG kTargetsMaxAgeMs = 60'000;

}  // namespace

bool WriteCycleTargets(const std::vector<std::wstring>& paths) {
    const std::wstring file = CycleTargetsPath();
    if (file.empty()) return false;
    if (paths.empty()) {
        DeleteFileW(file.c_str());
        return true;
    }

    std::wstring body;
    for (auto const& path : paths) {
        body += path;
        body += L'\n';
    }

    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;

    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    bool ok = WriteFile(handle, &bom, sizeof(bom), &written, nullptr) != 0
           && WriteFile(handle, body.data(),
                        static_cast<DWORD>(body.size() * sizeof(wchar_t)),
                        &written, nullptr) != 0;
    CloseHandle(handle);
    if (!ok) DeleteFileW(file.c_str());
    return ok;
}

std::vector<std::wstring> ConsumeCycleTargets() {
    std::vector<std::wstring> paths;
    const std::wstring file = CycleTargetsPath();
    if (file.empty()) return paths;

    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &attrs))
        return paths;

    ULARGE_INTEGER wrote{}, now{};
    wrote.LowPart  = attrs.ftLastWriteTime.dwLowDateTime;
    wrote.HighPart = attrs.ftLastWriteTime.dwHighDateTime;
    FILETIME nowFt{};
    GetSystemTimeAsFileTime(&nowFt);
    now.LowPart  = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    const ULONGLONG ageMs = now.QuadPart > wrote.QuadPart
                          ? (now.QuadPart - wrote.QuadPart) / 10000ULL : 0ULL;

    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        std::wstring body;
        wchar_t  buffer[512];
        DWORD    read = 0;
        while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0)
            body.append(buffer, read / sizeof(wchar_t));
        CloseHandle(handle);

        if (!body.empty() && body.front() == 0xFEFF) body.erase(body.begin());
        size_t start = 0;
        while (start < body.size()) {
            size_t end = body.find(L'\n', start);
            if (end == std::wstring::npos) end = body.size();
            std::wstring line = body.substr(start, end - start);
            while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n'))
                line.pop_back();
            if (!line.empty()) paths.push_back(std::move(line));
            start = end + 1;
        }
    }

    DeleteFileW(file.c_str());
    if (ageMs > kTargetsMaxAgeMs) paths.clear();
    return paths;
}

bool RestartInterfaceDevice(const std::wstring& interfacePath, DWORD* errorOut) {
    const auto results = RestartInterfaceDevices({interfacePath});
    if (results.empty()) {
        if (errorOut) *errorOut = ERROR_INVALID_PARAMETER;
        return false;
    }
    if (errorOut) *errorOut = results[0].error;
    return results[0].enabled && (results[0].cycled || results[0].restartRequested);
}

}
