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
//   SteamlessDeviceCycle.exe --register    create the on-demand scheduled
//                                          task pointing at this exe (admin)
//   SteamlessDeviceCycle.exe --unregister  delete the task (admin)
//
// The task has no triggers — it only runs when the tray app starts it via
// Task Scheduler, which requires no elevation for tasks the user authored.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>
#include "app/DeviceRestart.h"
#include "steam/SteamController.h"

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

static int CycleDevices() {
    bool any = false;
    // Bluetooth interfaces are skipped: restarting a BLE devnode is not a
    // replug — it can wedge the pairing, and it cannot free another
    // process's handle the way a USB re-enumeration does.
    for (const auto& path : SteamController::EnumerateAll())
        if (SteamController::TransportFromPath(path) != SteamController::Transport::Bluetooth
                && DeviceRestart::RestartInterfaceDevice(path))
            any = true;
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

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"--register"))   return RegisterTask();
    if (cmdLine && wcsstr(cmdLine, L"--unregister")) return UnregisterTask();
    return CycleDevices();
}
