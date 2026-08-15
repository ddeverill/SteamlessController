#include "WinPlatform.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace {
// Signal/exception handlers can only reach a plain function pointer, not a
// capturing lambda — a single process-wide slot is enough since there is
// exactly one WinPlatform per process, the same one-instance assumption the
// original ControllerManager crash-restore global made before this moved
// here.
std::function<void()>* g_emergencyRestore = nullptr;

LONG WINAPI CrashRestoreFilter(EXCEPTION_POINTERS*) {
    if (g_emergencyRestore && *g_emergencyRestore) (*g_emergencyRestore)();
    // Let Windows Error Reporting / debuggers see the crash as usual.
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

WinPlatform::WinPlatform() = default;

void WinPlatform::InstallCrashHandler(std::function<void()> emergencyRestore) {
    static std::function<void()> holder;  // outlives this call, process-lifetime
    holder = std::move(emergencyRestore);
    g_emergencyRestore = &holder;
    SetUnhandledExceptionFilter(CrashRestoreFilter);
}
