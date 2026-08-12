#include "DeviceRestart.h"
#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <cstdio>
#include <io.h>
#include <shlwapi.h>
#include <vector>
#pragma comment(lib, "shlwapi.lib")

namespace DeviceRestart {

// How long to let a disable settle before believing it was refused. Only ever
// spent on the path that is about to conclude "nothing went down", so a device
// that disables promptly pays none of it.
static constexpr int   DISABLE_SETTLE_POLLS   = 10;
static constexpr DWORD DISABLE_SETTLE_STEP_MS = 50;

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

const char* CycleKindName(CycleKind kind) {
    switch (kind) {
        case CycleKind::Cycled:      return "CYCLED";
        case CycleKind::Removed:     return "REMOVED AND RE-ADDED";
        case CycleKind::RestartOnly: return "FELL BACK TO RESTART";
        default:                     return "FAILED";
    }
}

const char* VetoTypeName(PNP_VETO_TYPE type) {
    switch (type) {
        case PNP_VetoLegacyDevice:          return "legacy device";
        case PNP_VetoPendingClose:          return "handle closing";
        case PNP_VetoWindowsApp:            return "application";
        case PNP_VetoWindowsService:        return "service";
        case PNP_VetoOutstandingOpen:       return "open handle";
        case PNP_VetoDevice:                return "device";
        case PNP_VetoDriver:                return "driver";
        case PNP_VetoIllegalDeviceRequest:  return "illegal request";
        case PNP_VetoInsufficientPower:     return "insufficient power";
        case PNP_VetoNonDisableable:        return "not disableable";
        case PNP_VetoLegacyDriver:          return "legacy driver";
        case PNP_VetoInsufficientRights:    return "insufficient rights";
        default:                            return "unknown";
    }
}

enum class RemoveOutcome { Error, Vetoed, Removed, RemovedButMissing };

// A node that cannot be located is not "enabled" — it is absent, and callers
// asking this question are checking whether a repair took.
static bool IsDevNodeDisabledById(const std::wstring& instanceId, ULONG locateFlags) {
    DEVINST devInst = 0;
    if (CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(instanceId.c_str()),
                           locateFlags) != CR_SUCCESS)
        return true;
    return IsDevNodeDisabled(devInst);
}

// Ask PnP to take the node out of the device tree.
//
// This is the only documented way to learn *why* a device will not go down: a
// refusal comes back as CR_REMOVE_VETOED carrying a veto type, which is what
// lets the app say "something is holding the controller" instead of assuming
// Steam is. Windows' own "Safely Remove Hardware" reports blockers this way.
//
// It is not a dry run — a granted query-remove really does remove the node, so
// this puts it back by re-enumerating the parent. That is a feature, not a
// hazard to work around: this only ever runs after a disable was already
// refused, and a granted removal breaks every open handle, which is exactly
// what the failed disable was supposed to do.
static RemoveOutcome QueryRemoveAndRestore(DEVINST devInst, CycleResult* out) {
    // Both of these have to be captured up front: once the node is gone, the
    // DEVINST is stale and there is nothing left to ask.
    wchar_t instanceId[MAX_DEVICE_ID_LEN] = {};
    if (CM_Get_Device_IDW(devInst, instanceId, ARRAYSIZE(instanceId), 0) != CR_SUCCESS)
        return RemoveOutcome::Error;

    DEVINST parent = 0;
    const bool haveParent = CM_Get_Parent(&parent, devInst, 0) == CR_SUCCESS;

    wchar_t       vetoName[MAX_PATH] = {};
    PNP_VETO_TYPE vetoType           = PNP_VetoTypeUnknown;
    const CONFIGRET cr = CM_Query_And_Remove_SubTreeW(
        devInst, &vetoType, vetoName, ARRAYSIZE(vetoName), CM_REMOVE_UI_NOT_OK);

    if (cr == CR_REMOVE_VETOED) {
        if (out) {
            out->vetoType = vetoType;
            out->vetoName = vetoName;
        }
        return RemoveOutcome::Vetoed;
    }
    if (cr != CR_SUCCESS)
        return RemoveOutcome::Error;

    // Removed. Bring it back by re-enumerating the parent — or the whole tree
    // if the parent could not be resolved, which is slower but always works.
    DEVINST rescanRoot = parent;
    if (!haveParent && CM_Locate_DevNodeW(&rescanRoot, nullptr, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return RemoveOutcome::RemovedButMissing;

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt) Sleep(500);
        CM_Reenumerate_DevNode(rescanRoot, CM_REENUMERATE_SYNCHRONOUS);
        DEVINST back = 0;
        if (CM_Locate_DevNodeW(&back, instanceId, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
            return RemoveOutcome::Removed;
    }
    return RemoveOutcome::RemovedButMissing;
}

// Take the node out of the device tree and let PnP build it again from scratch.
// The escape hatch for a devnode that will not enable in place: a node can sit
// at problem 22 with ConfigFlags clear, and for those DICS_ENABLE reports
// success and changes nothing however often it is asked, while a rebuild works.
//
// Returns true only when the node came back *and* came back enabled. Those are
// not the same thing: a persistent ConfigFlags disable is read again by the
// rebuilt node and disables it just as before, so a successful removal is no
// evidence on its own that anything was fixed — and treating it as such would
// throw away the pending record that is the only remaining way back.
static bool RebuildDevNode(const std::wstring& instanceId) {
    DEVINST devInst = 0;
    if (CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(instanceId.c_str()),
                           CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS)
        return false;
    if (QueryRemoveAndRestore(devInst, nullptr) != RemoveOutcome::Removed)
        return false;
    // Re-resolve: what came back is a different devnode instance.
    return !IsDevNodeDisabledById(instanceId, CM_LOCATE_DEVNODE_NORMAL);
}

// Take the devnode down and bring it back, invalidating every open handle to
// it — including the one Steam holds, which is the whole point of a cycle.
//
// Disable/enable rather than the politer DICS_PROPCHANGE "restart", because
// on a Bluetooth HID child that restart reports success while leaving the
// device and Steam's handle untouched.
//
// A disable is not, however, unconditional — it asks, and it can be refused.
// Measured against a process holding an open handle that never yields: the
// disable of this node is vetoed (ERROR_GEN_FAILURE after a ~6s query round
// trip), and so is a query-remove of this node *and* of the USB parent — both
// CR_REMOVE_VETOED / PNP_VetoOutstandingOpen after ~5.4s, with the parent's
// veto naming this child as the blocker. Consent propagates up the tree, so no
// devnode teardown reachable from user mode evicts such a handle. (Disabling
// the USB parent proves nothing either way: that node answers
// ERROR_NOT_SUPPORTED immediately, meaning "not disableable", not "vetoed".)
//
// What makes the cycle work against Steam is that Steam cooperates: it yields
// the device on DBT_DEVICEQUERYREMOVE and reopens on arrival, which is why the
// reopen is a race rather than a standoff. Against an uncooperative holder
// there is no eviction path, only detection — see the veto probe below.
//
// SetupDiCallClassInstaller's return value cannot be trusted here: a disable
// that Windows genuinely performed still came back as ERROR_INVALID_DATA on
// this Bluetooth HID child. So the devnode's own status is the source of
// truth, and the re-enable is never gated on what the disable claimed —
// getting that wrong strands the controller switched off, needing Device
// Manager to recover, which is far worse than failing to cycle at all.
static void CycleDevNode(HDEVINFO devs, SP_DEVINFO_DATA& devInfo, CycleResult& out) {
    // Write down what is about to be switched off, before switching it off. A
    // global disable lands in the registry immediately and survives this
    // process, so from here until the re-enable is confirmed there is a window
    // in which dying strands the device. The record is what closes it.
    //
    // The parent goes in alongside the node itself because the disable does not
    // reliably land where it is aimed: measured on the wired SC2026, disabling
    // the HID collection child left CONFIGFLAG_DISABLED on the USB parent
    // instead. Recording both costs nothing and makes recovery independent of
    // which node Windows actually marks.
    wchar_t instanceId[MAX_DEVICE_ID_LEN] = {};
    CM_Get_Device_IDW(devInfo.DevInst, instanceId, ARRAYSIZE(instanceId), 0);

    wchar_t parentId[MAX_DEVICE_ID_LEN] = {};
    if (DEVINST parent = 0; CM_Get_Parent(&parent, devInfo.DevInst, 0) == CR_SUCCESS)
        CM_Get_Device_IDW(parent, parentId, ARRAYSIZE(parentId), 0);

    ArmPendingDisable(instanceId, parentId);

    // Some devices reject a global scope change and only accept a
    // config-specific one, so try both before believing a disable was refused.
    if (!SetDeviceState(devs, devInfo, DICS_DISABLE, DICS_FLAG_GLOBAL))
        SetDeviceState(devs, devInfo, DICS_DISABLE, DICS_FLAG_CONFIGSPECIFIC);

    // Did it actually go down? This, not the return code above, decides
    // whether a real cycle happened.
    //
    // Polled rather than read once. The devnode's status does not always
    // reflect the disable by the time the call returns, and reading it too
    // early is not a harmless misreading: "nothing went down" skips the
    // re-enable below entirely and clears the pending record, so a disable that
    // lands a moment later is left in place with nothing tracking it and a
    // cycle verdict claiming the opposite. Reported from the field as
    // collections sitting at problem 22 under a FELL BACK TO RESTART verdict.
    bool wentDown = false;
    for (int i = 0; i < DISABLE_SETTLE_POLLS && !wentDown; ++i) {
        if (i) Sleep(DISABLE_SETTLE_STEP_MS);
        wentDown = IsDevNodeDisabled(devInfo.DevInst);
    }

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
        // Enabling refused to take. Retrying it again achieves nothing — field
        // reports have DICS_ENABLE returning success while the node stays at
        // problem 22 however often it is asked — so take the node out of the
        // tree and let it be built fresh instead. A disabled devnode has no
        // open handles by definition, which is what makes a query-remove
        // viable here when it would be vetoed on a live one.
        const DWORD enableError = GetLastError();
        if (RebuildDevNode(instanceId)) {
            DisarmPendingDisable();
            out.kind  = CycleKind::Removed;
            out.error = ERROR_SUCCESS;
            return;
        }

        // The controller is stranded disabled — the caller must surface this,
        // it is not a quiet failure. The pending record deliberately stays
        // armed: this is exactly the state the next run has to clean up.
        out.kind  = CycleKind::Failed;
        out.error = enableError;
        return;
    }

    // Confirmed back. Nothing left for a later run to undo.
    DisarmPendingDisable();

    if (wentDown) {
        out.kind = CycleKind::Cycled;
        return;
    }

    // Nothing was taken down, so no handle was broken. Find out why, and take
    // the removal route if PnP will grant it — a plain restart here is known
    // to change nothing at all.
    out.error = ERROR_NOT_SUPPORTED;
    switch (QueryRemoveAndRestore(devInfo.DevInst, &out)) {
        case RemoveOutcome::Removed:
            out.kind  = CycleKind::Removed;
            out.error = ERROR_SUCCESS;
            return;
        case RemoveOutcome::RemovedButMissing:
            // Handles did die with the node, so as a cycle this worked, but the
            // device has not come back yet. Report it so the caller can say so.
            out.kind  = CycleKind::Removed;
            out.error = ERROR_DEVICE_NOT_AVAILABLE;
            return;
        default:
            break;
    }

    // Vetoed or unanswerable — fall back to asking for a plain restart, which
    // is enough on USB when nothing else holds the device.
    out.kind = SetDeviceState(devs, devInfo, DICS_PROPCHANGE, DICS_FLAG_GLOBAL)
                   ? CycleKind::RestartOnly
                   : CycleKind::Failed;
}

bool RestartInterfaceDevice(const std::wstring& interfacePath, CycleResult& result) {
    result = CycleResult{};

    HDEVINFO devs = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (devs == INVALID_HANDLE_VALUE) {
        result.error = GetLastError();
        return false;
    }

    SP_DEVICE_INTERFACE_DATA ifData{};
    ifData.cbSize = sizeof(ifData);
    if (SetupDiOpenDeviceInterfaceW(devs, interfacePath.c_str(), 0, &ifData)) {
        // Query with no output buffer purely to resolve the owning devnode —
        // the call fails with ERROR_INSUFFICIENT_BUFFER but still fills devInfo.
        SP_DEVINFO_DATA devInfo{};
        devInfo.cbSize = sizeof(devInfo);
        SetupDiGetDeviceInterfaceDetailW(devs, &ifData, nullptr, 0, nullptr, &devInfo);

        if (devInfo.DevInst != 0)
            CycleDevNode(devs, devInfo, result);
        else
            result.error = GetLastError();
    } else {
        result.error = GetLastError();
    }

    SetupDiDestroyDeviceInfoList(devs);
    result.ticks = GetTickCount64();
    return result.kind != CycleKind::Failed;
}

bool RestartInterfaceDevice(const std::wstring& interfacePath, DWORD* errorOut) {
    CycleResult result;
    const bool ok = RestartInterfaceDevice(interfacePath, result);
    if (errorOut) *errorOut = result.error;
    return ok;
}

// ---------------------------------------------------------------------------
// Cross-process status handoff
// ---------------------------------------------------------------------------

static std::wstring StatusPath() {
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return L"";
    const std::wstring dir = std::wstring(local) + L"\\SteamlessController";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\cycle.status";
}

bool WriteCycleStatus(const CycleResult& result) {
    const std::wstring path = StatusPath();
    if (path.empty()) return false;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8") != 0 || !f) return false;

    fwprintf(f, L"kind=%d\n",    static_cast<int>(result.kind));
    fwprintf(f, L"error=%lu\n",  result.error);
    fwprintf(f, L"veto=%d\n",    static_cast<int>(result.vetoType));
    fwprintf(f, L"self=%d\n",    result.selfHeld ? 1 : 0);
    fwprintf(f, L"ticks=%llu\n", result.ticks);
    fwprintf(f, L"name=%s\n",       result.vetoName.c_str());
    fwprintf(f, L"holders=%s\n",    result.holders.c_str());
    fwprintf(f, L"holdersAll=%s\n", result.holdersAll.c_str());
    fwprintf(f, L"scan=%s\n",       result.scanInfo.c_str());
    fclose(f);
    return true;
}

bool ReadCycleStatus(CycleResult& result) {
    const std::wstring path = StatusPath();
    if (path.empty()) return false;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") != 0 || !f) return false;

    result = CycleResult{};
    wchar_t line[512];
    while (fgetws(line, ARRAYSIZE(line), f)) {
        // Trim the trailing newline so a value never carries it.
        const size_t len = wcslen(line);
        if (len && line[len - 1] == L'\n') line[len - 1] = L'\0';

        int         value = 0;
        unsigned long ul  = 0;
        unsigned long long ull = 0;
        if (swscanf_s(line, L"kind=%d", &value) == 1)
            result.kind = static_cast<CycleKind>(value);
        else if (swscanf_s(line, L"error=%lu", &ul) == 1)
            result.error = ul;
        else if (swscanf_s(line, L"veto=%d", &value) == 1)
            result.vetoType = static_cast<PNP_VETO_TYPE>(value);
        else if (swscanf_s(line, L"self=%d", &value) == 1)
            result.selfHeld = value != 0;
        else if (swscanf_s(line, L"ticks=%llu", &ull) == 1)
            result.ticks = ull;
        else if (wcsncmp(line, L"name=", 5) == 0)
            result.vetoName = line + 5;
        else if (wcsncmp(line, L"holdersAll=", 11) == 0)
            result.holdersAll = line + 11;
        else if (wcsncmp(line, L"holders=", 8) == 0)
            result.holders = line + 8;
        else if (wcsncmp(line, L"scan=", 5) == 0)
            result.scanInfo = line + 5;
    }
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Crash-safe disable
// ---------------------------------------------------------------------------

static std::wstring PendingPath() {
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return L"";
    const std::wstring dir = std::wstring(local) + L"\\SteamlessController";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\pending-disable";
}

bool ArmPendingDisable(const std::wstring& instanceId,
                       const std::wstring& parentInstanceId) {
    const std::wstring path = PendingPath();
    if (path.empty() || instanceId.empty()) return false;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8") != 0 || !f) return false;

    // Outermost node first: reconciliation walks the file in order, and a
    // disabled parent has to come back before its child can be looked at.
    if (!parentInstanceId.empty())
        fwprintf(f, L"%s\n", parentInstanceId.c_str());
    fwprintf(f, L"%s\n", instanceId.c_str());

    // The whole point of this file is to outlive an abrupt death, so it has to
    // reach the disk before the disable it is guarding — buffered in this
    // process is no better than not written at all.
    fflush(f);
    _commit(_fileno(f));
    fclose(f);
    return true;
}

bool DisarmPendingDisable() {
    const std::wstring path = PendingPath();
    if (path.empty()) return false;
    return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

static std::vector<std::wstring> ReadPendingIds() {
    std::vector<std::wstring> ids;
    const std::wstring path = PendingPath();
    if (path.empty()) return ids;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") != 0 || !f) return ids;

    wchar_t line[MAX_DEVICE_ID_LEN + 2];
    while (fgetws(line, ARRAYSIZE(line), f)) {
        size_t len = wcslen(line);
        while (len && (line[len - 1] == L'\n' || line[len - 1] == L'\r'))
            line[--len] = L'\0';
        if (len) ids.emplace_back(line);
    }
    fclose(f);
    return ids;
}

bool HasPendingDisable() {
    return !ReadPendingIds().empty();
}

// Bring one devnode back, addressed by instance ID rather than by an interface
// path — the interfaces are gone while the node is disabled, so the path that
// started the cycle cannot be used to finish it.
static bool EnableByInstanceId(const std::wstring& id, bool& wasDisabled) {
    wasDisabled = false;

    DEVINST devInst = 0;
    // PHANTOM rather than NORMAL: the node may not be started, which is the
    // entire situation being repaired.
    if (CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(id.c_str()),
                           CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS)
        return false;
    if (!IsDevNodeDisabled(devInst)) return false;
    wasDisabled = true;

    HDEVINFO devs = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (devs == INVALID_HANDLE_VALUE) return false;

    bool enabled = false;
    SP_DEVINFO_DATA devInfo{};
    devInfo.cbSize = sizeof(devInfo);
    if (SetupDiOpenDeviceInfoW(devs, id.c_str(), nullptr, 0, &devInfo)) {
        for (int attempt = 0; attempt < 3 && !enabled; ++attempt) {
            if (attempt) Sleep(500);
            SetDeviceState(devs, devInfo, DICS_ENABLE, DICS_FLAG_GLOBAL);
            if (IsDevNodeDisabled(devInfo.DevInst))
                SetDeviceState(devs, devInfo, DICS_ENABLE, DICS_FLAG_CONFIGSPECIFIC);
            enabled = !IsDevNodeDisabled(devInfo.DevInst);
        }
    }
    SetupDiDestroyDeviceInfoList(devs);

    // Enabling in place does not always work. A devnode can sit at problem 22
    // with ConfigFlags clear — a disable that was never written to the registry
    // — and for those, DICS_ENABLE reports success and changes nothing, no
    // matter how many times it is asked. Rebuilding the node does work, and
    // costs nothing extra when the enable above already succeeded.
    if (!enabled)
        enabled = RebuildDevNode(id);
    return enabled;
}

int ReconcilePendingDisables(std::wstring* report) {
    const std::vector<std::wstring> ids = ReadPendingIds();
    if (ids.empty()) return 0;

    int recovered = 0;
    std::wstring text;
    for (const auto& id : ids) {
        bool wasDisabled = false;
        const bool ok = EnableByInstanceId(id, wasDisabled);
        if (!wasDisabled) continue;  // came back on its own, or never went down

        if (ok) ++recovered;
        if (!text.empty()) text += L"; ";
        text += id + (ok ? L" -> re-enabled" : L" -> STILL DISABLED");
    }

    // Drop the record either way. A node that could not be re-enabled here will
    // not be re-enabled by trying again on the next launch, and keeping the
    // file would turn one stranded device into a permanent startup ritual —
    // the failure is surfaced through the log instead.
    DisarmPendingDisable();

    if (report) *report = text;
    return recovered;
}

std::wstring FindDisabledControllerNode() {
    // Every enumerator the controller can appear under. HID matters most and is
    // the easiest to forget: a cycle disables the HID collection child, so that
    // is the node most likely to be found switched off — its USB parent sits
    // one level up and stays perfectly healthy. Non-present nodes are included
    // deliberately, since a disabled devnode is exactly the case where the
    // device is attached but not started.
    for (const wchar_t* enumerator : { L"HID", L"USB", L"BTHLEDEVICE", L"BTHENUM" }) {
        ULONG len = 0;
        if (CM_Get_Device_ID_List_SizeW(&len, enumerator,
                                        CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS
            || len < 2)
            continue;

        std::vector<wchar_t> buf(len);
        if (CM_Get_Device_ID_ListW(enumerator, buf.data(), len,
                                   CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS)
            continue;

        for (const wchar_t* id = buf.data(); *id; id += wcslen(id) + 1) {
            // Valve's vendor ID, however the enumerator spells the token.
            if (!StrStrIW(id, L"VID_28DE") && !StrStrIW(id, L"VID&0228DE")) continue;

            DEVINST devInst = 0;
            if (CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(id),
                                   CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS)
                continue;
            if (IsDevNodeDisabled(devInst)) return id;
        }
    }
    return L"";
}

}
