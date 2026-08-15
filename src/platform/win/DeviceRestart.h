#pragma once
#include <Windows.h>
#include <cfgmgr32.h>
#include <string>

namespace DeviceRestart {

// What a cycle actually did to the devnode — which decides whether anyone
// else's handles died with it.
enum class CycleKind {
    Failed      = 0,  // nothing happened; the device may not even be there
    RestartOnly = 1,  // disable refused, so only a polite PROPCHANGE restart ran.
                      // Other processes keep their handles: this cycle changed nothing.
    Cycled      = 2,  // devnode went down and came back — every handle invalidated
    Removed     = 3,  // query-remove was granted and the node re-enumerated; same effect
};

// The verdict of one cycle. Written by the elevated helper, read by the tray
// app, which is the process that has to explain a failure to the user.
struct CycleResult {
    CycleKind     kind     = CycleKind::Failed;
    DWORD         error    = ERROR_SUCCESS;
    // Only filled on RestartOnly: PnP's reason for refusing to take the node
    // down. PNP_VetoTypeUnknown means the question was never answered.
    PNP_VETO_TYPE vetoType = PNP_VetoTypeUnknown;
    // For an app or service veto this names the culprit. For an outstanding-open
    // veto Windows reports the device, not the process holding it, so treat a
    // name as a bonus rather than something to rely on.
    std::wstring  vetoName;
    // Processes found holding the device open, "name (pid)" joined by "; ".
    // Filled only on the veto path and only when the scan found something —
    // it needs elevation, so empty means "not established", never "nobody".
    //
    // `holders` is what the user is told: the expected holders (Steam, us) are
    // stripped out, because naming them is not advice anyone can act on.
    // `holdersAll` and `scanInfo` keep the unfiltered picture for the event log,
    // so a support log carries what actually happened rather than a conclusion.
    std::wstring  holders;
    std::wstring  holdersAll;
    std::wstring  scanInfo;
    ULONGLONG     ticks    = 0;  // GetTickCount64() when the cycle finished
    // Every handle found on the device belonged to this app. PnP refused the
    // teardown because of something we are holding ourselves, so cycling again
    // cannot help: the answer is to let go, not to restart the devnode. Only
    // set when the scan ran to completion — a partial scan cannot prove that
    // nobody else has it open.
    bool          selfHeld = false;

    // Nothing that could invalidate another process's handle took place, so
    // repeating the cycle will achieve exactly as much as the last one did.
    bool BrokeNoHandles() const {
        return kind == CycleKind::RestartOnly || kind == CycleKind::Failed;
    }

    // PnP told us something is actively using the device. This is the
    // distinction the app could not previously make: "someone holds the
    // controller" rather than the assumed "Steam will not let go".
    bool HeldByAnotherProcess() const {
        return kind == CycleKind::RestartOnly
            && (vetoType == PNP_VetoOutstandingOpen
                || vetoType == PNP_VetoPendingClose
                || vetoType == PNP_VetoWindowsApp
                || vetoType == PNP_VetoWindowsService);
    }

    // True when vetoName identifies a process rather than the device itself.
    bool VetoNamesTheHolder() const {
        return !vetoName.empty()
            && (vetoType == PNP_VetoWindowsApp || vetoType == PNP_VetoWindowsService);
    }
};

const char* CycleKindName(CycleKind kind);
const char* VetoTypeName(PNP_VETO_TYPE type);

// Restarts the PnP device node backing an HID interface path — equivalent to
// unplugging and replugging the device. Every process's open handles to it
// (including Steam's) are invalidated, and the re-arrival becomes an open
// race that whoever reacts to the arrival notification first wins.
//
// Requires administrator rights; fails with ERROR_ACCESS_DENIED in errorOut
// when the process is not elevated.
bool RestartInterfaceDevice(const std::wstring& interfacePath, DWORD* errorOut = nullptr);
bool RestartInterfaceDevice(const std::wstring& interfacePath, CycleResult& result);

// The cycle runs in the elevated helper but the tray app owns the user-facing
// story, so the verdict crosses processes through a small file next to the
// logs. Read returns false when no status has been written this boot.
bool WriteCycleStatus(const CycleResult& result);
bool ReadCycleStatus(CycleResult& result);

// ---------------------------------------------------------------------------
// Crash-safe disable
// ---------------------------------------------------------------------------
//
// A global disable is written straight into the devnode's registry ConfigFlags,
// so it outlives the process that asked for it — and a reboot, and a replug.
// The re-enable that undoes it lives a second or so later in the same function,
// which is fine right up until the process does not survive that second.
//
// That is not hypothetical: a cycle on the elevated in-process path runs inside
// the long-lived tray app, and killing it mid-cycle strands the controller
// switched off with nothing left running to put it back. Recovering needs
// Device Manager, which is not something a user should have to discover.
//
// So the intent to disable is recorded durably *before* the disable, and
// cleared only once the devnode confirms it is back. Anything left behind is a
// cycle that did not finish, and the next run undoes it.

// Record that these devnodes are about to be disabled. Call immediately before
// the disable; the record is flushed to disk before this returns.
bool ArmPendingDisable(const std::wstring& instanceId,
                       const std::wstring& parentInstanceId);

// Drop the record. Call only once the devnode is confirmed enabled again.
bool DisarmPendingDisable();

// True when a previous run left a disable unfinished. Cheap; safe unelevated.
bool HasPendingDisable();

// Re-enable whatever a previous run left disabled, then drop the record.
// Requires elevation — the tray app routes this through the helper.
// Returns the number of devnodes actually brought back; `report` receives a
// human-readable summary for the event log.
int ReconcilePendingDisables(std::wstring* report = nullptr);

// Any Valve controller devnode sitting in CM_PROB_DISABLED, whoever switched it
// off — us, Device Manager, or a tool that hides HID devices. Returns an empty
// string when nothing is disabled.
//
// A disabled devnode publishes no interfaces, so from inside this app it is
// indistinguishable from a controller that was never plugged in: enumeration
// simply returns nothing. That is a bad thing to be silent about, because the
// device still lights up and still makes Windows chime when it is plugged in,
// so the user has every reason to believe it is fine and we are broken.
// Read-only and safe unelevated.
std::wstring FindDisabledControllerNode();

}
