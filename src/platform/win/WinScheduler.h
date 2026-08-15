#pragma once
#include "core/iface/IScheduler.h"

// Backed by SetTimer(nullptr, ...) — a timer not tied to any window, whose
// WM_TIMER still flows through the calling thread's message queue and
// invokes the TIMERPROC callback directly when DispatchMessage processes it.
// That means this needs no pump call of its own: as long as the thread that
// created a timer keeps running an ordinary GetMessage/DispatchMessage loop
// (TrayApp::Run() already does), timers just fire.
//
// Not currently exercised: ControllerManager never calls IPlatform::Scheduler()
// (TrayApp still uses its own SetTimer(m_hwnd, ...) calls directly, unchanged
// from before the port). This exists so WinPlatform has a real interface
// slot. Callback storage is process-global (in the .cpp) rather than a
// member, since TIMERPROC is a plain function pointer with no user-data
// parameter to carry a `this` through — harmless given there is only ever
// one WinScheduler per process.
class WinScheduler : public IScheduler {
public:
    TimerId After(std::chrono::milliseconds delay, std::function<void()> fn) override;
    TimerId Every(std::chrono::milliseconds interval, std::function<void()> fn) override;
    void Cancel(TimerId id) override;
    void Post(std::function<void()> fn) override;
};
