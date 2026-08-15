#pragma once
#include "core/iface/IScheduler.h"
#include <map>
#include <mutex>

// Queues timers/Post()'d callbacks thread-safely, but — unlike a "real"
// scheduler — never executes them on a thread of its own. Execution only
// happens via RunDuePending(), called from Daemon::Run()'s single loop.
// This is what makes IScheduler's "everything runs on one thread" contract
// actually hold: LinuxSteamWatcher and LinuxDeviceMonitor each run their own
// background thread and call Post()/After() to hand a callback to the main
// thread — if this class ran callbacks on a thread of its own instead, two
// background threads could each end up driving ControllerManager
// concurrently, which is exactly the kind of unsynchronized-access bug
// that showed up (as a segfault) before this class was written this way.
class LinuxScheduler : public IScheduler {
public:
    TimerId After(std::chrono::milliseconds delay, std::function<void()> fn) override;
    TimerId Every(std::chrono::milliseconds interval, std::function<void()> fn) override;
    void Cancel(TimerId id) override;
    void Post(std::function<void()> fn) override;

    // Runs every timer whose due time has passed (on the calling thread —
    // must always be the daemon's main thread). Returns how many
    // milliseconds until the next pending timer is due, or -1 if none are
    // pending, so the caller can size its own poll() timeout around it.
    int RunDuePending();

private:
    struct Timer {
        std::chrono::steady_clock::time_point due;
        std::chrono::milliseconds             interval{0};  // 0 = one-shot
        std::function<void()>                 fn;
        bool                                  cancelled = false;
    };

    std::mutex               m_mutex;
    std::map<TimerId, Timer> m_timers;
    TimerId                  m_nextId = 1;
};
