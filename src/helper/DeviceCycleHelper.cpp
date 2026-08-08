// SteamlessDeviceCycle — tiny elevated on-demand helper.
//
// Restarts the SC2026 device nodes (equivalent to unplug/replug) so HID
// handles held by other processes — Steam's — are invalidated and the tray
// app can win the re-open race. Kept separate from the tray app so the app
// itself never runs elevated: an elevated foreground window blocks synthetic
// input from unelevated processes (UIPI), which froze Steam Input's desktop
// cursor whenever the elevated tray menu was open.
//
// Usage:
//   SteamlessDeviceCycle.exe               cycle the controller devices
//   SteamlessDeviceCycle.exe --reconcile   re-enable a devnode left disabled
//                                          by an interrupted cycle (admin)
//   SteamlessDeviceCycle.exe --register    create the on-demand scheduled
//                                          task pointing at this exe (admin)
//   SteamlessDeviceCycle.exe --unregister  delete the task (admin)
//
// The task has no triggers — it only runs when the tray app starts it via
// Task Scheduler, which requires no elevation for tasks the user authored.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include "app/DeviceRestart.h"
#include "app/HandleFinder.h"
#include "steam/SteamController.h"

// The helper runs windowless from Task Scheduler, so a failed cycle is
// otherwise completely silent. It cannot share the tray app's events.log —
// that is held open with _SH_DENYWR — so it keeps its own next to it.
static void CycleLog(const char* fmt, ...) {
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return;
    const std::wstring dir  = std::wstring(local) + L"\\SteamlessController";
    CreateDirectoryW(dir.c_str(), nullptr);

    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + L"\\cycle.log").c_str(), L"a") != 0 || !f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u  ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fputc('\n', f);
    fclose(f);
}

static constexpr wchar_t CYCLE_TASK_NAME[] = L"SteamlessControllerDeviceCycle";
// Startup task name used briefly by a pre-release build that ran the tray app
// elevated at logon; removed during --register so it can't resurrect the bug.
static constexpr wchar_t LEGACY_STARTUP_TASK_NAME[] = L"SteamlessController";

static bool RunToolHidden(std::wstring cmdline) {
    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// Undo a disable that an earlier run committed but never got to reverse.
// Elevated, like everything else here — the tray app cannot do this itself.
static int ReconcilePending() {
    std::wstring report;
    const int recovered = DeviceRestart::ReconcilePendingDisables(&report);
    if (report.empty()) return 0;  // nothing was pending, or it recovered itself

    CycleLog("RECONCILE: %ls", report.c_str());
    // A device left switched off is the failure this whole mechanism exists to
    // prevent, so say so loudly rather than exiting quietly.
    if (report.find(L"STILL DISABLED") != std::wstring::npos) {
        CycleLog("RECONCILE: a devnode could not be re-enabled — it needs "
                 "Device Manager, or pnputil /enable-device");
        return 1;
    }
    // Deliberately not "from an interrupted cycle": the same record is written
    // when the user asks for a disabled device to be switched back on, and from
    // here the two are indistinguishable.
    CycleLog("RECONCILE: recovered %d devnode(s) left disabled", recovered);
    return 0;
}

static int CycleDevices() {
    // An interrupted cycle leaves the controller switched off, and a switched
    // off controller enumerates nothing — so without this the enumeration below
    // finds no interfaces and the cycle silently does nothing, which looks
    // exactly like a controller that is simply not plugged in.
    ReconcilePending();

    bool any = false;
    const auto paths = SteamController::EnumerateAll();
    CycleLog("START: %zu interface(s) to cycle", paths.size());

    // Summarised for the tray app, which has to explain any failure to the
    // user. Best outcome wins: if even one interface genuinely went down,
    // handles were broken and this was not a do-nothing cycle.
    DeviceRestart::CycleResult summary;

    for (const auto& path : paths) {
        DeviceRestart::CycleResult r;
        const bool ok = DeviceRestart::RestartInterfaceDevice(path, r);

        // A veto means some process is sitting on the device and nothing we can
        // do to the devnode will dislodge it — so the only useful thing left is
        // to say who. PnP will not tell us, hence the handle scan. Confined to
        // this path because it is expensive and this process exits right after.
        if (r.HeldByAnotherProcess()) {
            // Publish the veto verdict before the scan starts. The tray app
            // reads this file on its own timer, and a slow scan must not leave
            // it looking at the previous cycle's result.
            r.ticks = GetTickCount64();
            DeviceRestart::WriteCycleStatus(r);

            const auto scan = HandleFinder::FindDeviceHolders(path);

            // Two holders are always in this list and neither is worth telling
            // the user about.
            //
            // steam.exe holds the controller by design and is demonstrably not
            // the blocker — it yields on query-remove. Naming it would
            // resurrect the exact bad advice this path exists to replace.
            //
            // SteamlessController.exe is us: the tray app's retry timer fires
            // mid-cycle and reopens the device before the cycle it asked for
            // has finished. "Close SteamlessController" is not advice anyone
            // can act on. (That reopen is a real problem in its own right, not
            // just a reporting wart — it means we may be vetoing our own
            // cycle.)
            static const wchar_t* kNotWorthNaming[] = {
                L"steam.exe", L"SteamlessController.exe", L"SteamlessDeviceCycle.exe",
            };
            std::vector<HandleFinder::Holder> culprits;
            for (const auto& h : scan.holders) {
                bool skip = false;
                for (const wchar_t* name : kNotWorthNaming)
                    if (_wcsicmp(h.image.c_str(), name) == 0) { skip = true; break; }
                if (!skip) culprits.push_back(h);
            }
            r.holders    = HandleFinder::DescribeHolders(culprits);
            r.holdersAll = HandleFinder::DescribeHolders(scan.holders);

            // Kept as a ready-made line so the tray app can put it straight
            // into the event log the user can actually reach and send.
            wchar_t info[512];
            swprintf_s(info,
                       L"%s in %lu ms, candidates=%u, queried=%u, target='%s'",
                       scan.completed ? L"completed" : L"hit deadline",
                       scan.elapsedMs, scan.examined, scan.queried,
                       scan.targetName.empty() ? L"(unresolved)" : scan.targetName.c_str());
            r.scanInfo = info;

            CycleLog("SCAN: %ls typeIndex=%u", r.scanInfo.c_str(), scan.typeIndex);

            CycleLog("HOLDERS: all=[%ls] reportable=[%ls]%s",
                     r.holdersAll.empty() ? L"none" : r.holdersAll.c_str(),
                     r.holders.empty()    ? L"none" : r.holders.c_str(),
                     scan.completed ? "" : "  (scan incomplete — list may be partial)");
        }

        if (r.kind == DeviceRestart::CycleKind::RestartOnly
                && r.vetoType != PNP_VetoTypeUnknown) {
            CycleLog("%s (transport=%s err=%lu veto=%s '%ls') %ls",
                     DeviceRestart::CycleKindName(r.kind),
                     SteamController::TransportName(SteamController::TransportFromPath(path)),
                     r.error, DeviceRestart::VetoTypeName(r.vetoType),
                     r.vetoName.c_str(), path.c_str());
        } else {
            CycleLog("%s (transport=%s err=%lu) %ls",
                     DeviceRestart::CycleKindName(r.kind),
                     SteamController::TransportName(SteamController::TransportFromPath(path)),
                     r.error, path.c_str());
        }

        // Keep the most informative verdict: a real cycle outranks a fallback,
        // and among fallbacks one that named a blocker outranks a silent one.
        const bool better = r.kind > summary.kind
            || (r.kind == summary.kind
                && summary.vetoType == PNP_VetoTypeUnknown
                && r.vetoType != PNP_VetoTypeUnknown);
        if (better) summary = r;
        if (ok) any = true;
    }

    if (summary.kind == DeviceRestart::CycleKind::Removed
            && summary.error == ERROR_DEVICE_NOT_AVAILABLE)
        CycleLog("WARNING: the device was removed but has not re-appeared yet; "
                 "a rescan or replug may be needed");
    if (summary.BrokeNoHandles())
        CycleLog("NOTE: no handle was invalidated — repeating this cycle cannot help");

    summary.ticks = GetTickCount64();
    DeviceRestart::WriteCycleStatus(summary);

    CycleLog("DONE: exit=%d", any ? 0 : 1);
    return any ? 0 : 1;
}

static int RegisterTask() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    // Trigger-less task definition: on-demand only, highest privileges.
    // Registered from XML because the schtasks command line cannot express
    // "no trigger" (and its /SD date parsing is locale-dependent).
    std::wstring xml;
    xml += L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n";
    xml += L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n";
    xml += L"  <RegistrationInfo>\r\n";
    xml += L"    <Description>Restarts the Steam Controller device on demand for SteamlessController.</Description>\r\n";
    xml += L"  </RegistrationInfo>\r\n";
    xml += L"  <Principals>\r\n";
    xml += L"    <Principal id=\"Author\">\r\n";
    xml += L"      <LogonType>InteractiveToken</LogonType>\r\n";
    xml += L"      <RunLevel>HighestAvailable</RunLevel>\r\n";
    xml += L"    </Principal>\r\n";
    xml += L"  </Principals>\r\n";
    xml += L"  <Settings>\r\n";
    xml += L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n";
    xml += L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n";
    xml += L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n";
    xml += L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n";
    xml += L"    <Enabled>true</Enabled>\r\n";
    xml += L"    <ExecutionTimeLimit>PT1M</ExecutionTimeLimit>\r\n";
    xml += L"  </Settings>\r\n";
    xml += L"  <Actions Context=\"Author\">\r\n";
    xml += L"    <Exec><Command>";
    xml += exe;
    xml += L"</Command></Exec>\r\n";
    xml += L"  </Actions>\r\n";
    xml += L"</Task>\r\n";

    wchar_t tempDir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tempDir)) return 1;
    std::wstring xmlPath = std::wstring(tempDir) + L"SteamlessCycleTask.xml";

    HANDLE f = CreateFileW(xmlPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 1;
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    WriteFile(f, &bom, sizeof(bom), &written, nullptr);
    WriteFile(f, xml.c_str(),
              static_cast<DWORD>(xml.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(f);

    std::wstring cmd = L"schtasks.exe /Create /F /TN \"";
    cmd += CYCLE_TASK_NAME;
    cmd += L"\" /XML \"";
    cmd += xmlPath;
    cmd += L"\"";
    const bool ok = RunToolHidden(std::move(cmd));
    DeleteFileW(xmlPath.c_str());

    // Best-effort cleanup of the pre-release elevated startup task.
    std::wstring legacy = L"schtasks.exe /Delete /F /TN \"";
    legacy += LEGACY_STARTUP_TASK_NAME;
    legacy += L"\"";
    RunToolHidden(std::move(legacy));

    return ok ? 0 : 1;
}

static int UnregisterTask() {
    std::wstring cmd = L"schtasks.exe /Delete /F /TN \"";
    cmd += CYCLE_TASK_NAME;
    cmd += L"\"";
    return RunToolHidden(std::move(cmd)) ? 0 : 1;
}

// Run the holder scan on its own, without needing the controller to be stuck.
// The scan is the one piece here that cannot be reasoned about from its output
// alone — an empty list has several causes — so it needs to be runnable on
// demand rather than only in the failure it is meant to explain.
static int FindHoldersOnly() {
    CycleLog("FIND: holder scan requested directly");
    for (const auto& path : SteamController::EnumerateAll()) {
        const auto scan = HandleFinder::FindDeviceHolders(path);
        CycleLog("FIND: %s in %lu ms, target='%ls' typeIndex=%u candidates=%u queried=%u",
                 scan.completed ? "completed" : "HIT DEADLINE",
                 scan.elapsedMs, scan.targetName.c_str(),
                 scan.typeIndex, scan.examined, scan.queried);
        const std::wstring holders = HandleFinder::DescribeHolders(scan.holders);
        CycleLog("FIND: holders=[%ls] %ls",
                 holders.empty() ? L"none" : holders.c_str(), path.c_str());
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"--register"))     return RegisterTask();
    if (cmdLine && wcsstr(cmdLine, L"--unregister"))   return UnregisterTask();
    if (cmdLine && wcsstr(cmdLine, L"--find-holders")) return FindHoldersOnly();
    if (cmdLine && wcsstr(cmdLine, L"--reconcile"))    return ReconcilePending();
    return CycleDevices();
}
