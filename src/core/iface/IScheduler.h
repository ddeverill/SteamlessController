#pragma once
#include <chrono>
#include <cstdint>
#include <functional>

// Timers and cross-thread marshalling for the single-threaded event loop that
// owns ControllerService. Windows: SetTimer/PostMessage on the TrayApp
// window. Linux: a timerfd + eventfd inside the daemon's poll() loop. Every
// ControllerService callback ends up running on exactly one thread — the
// Windows UI thread, or the daemon's single main thread — via this
// interface, which is what lets ControllerService itself stay free of any
// locking.
class IScheduler {
public:
    using TimerId = uint32_t;

    virtual ~IScheduler() = default;

    virtual TimerId After(std::chrono::milliseconds delay, std::function<void()> fn) = 0;
    virtual TimerId Every(std::chrono::milliseconds interval, std::function<void()> fn) = 0;
    virtual void Cancel(TimerId id) = 0;

    // Marshal a callback (typically one invoked from a read thread) onto the
    // scheduler's own thread.
    virtual void Post(std::function<void()> fn) = 0;
};
