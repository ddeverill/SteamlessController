#include "WinScheduler.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <map>
#include <mutex>

namespace {

std::mutex                               g_mutex;
std::map<UINT_PTR, std::function<void()>> g_oneShot;    // erased after firing
std::map<UINT_PTR, std::function<void()>> g_repeating;  // stays until Cancel()
UINT_PTR                                  g_nextId = 1;

void CALLBACK TimerProc(HWND, UINT, UINT_PTR id, DWORD) {
    std::function<void()> fn;
    bool repeating = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (auto it = g_oneShot.find(id); it != g_oneShot.end()) {
            fn = it->second;
            g_oneShot.erase(it);
            KillTimer(nullptr, id);
        } else if (auto rit = g_repeating.find(id); rit != g_repeating.end()) {
            fn = rit->second;
            repeating = true;
        }
    }
    (void)repeating;  // WM_TIMER re-fires on its own interval; nothing else to do here
    if (fn) fn();
}

}  // namespace

WinScheduler::TimerId WinScheduler::After(std::chrono::milliseconds delay, std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_mutex);
    const UINT_PTR id = g_nextId++;
    g_oneShot[id] = std::move(fn);
    SetTimer(nullptr, id, static_cast<UINT>(delay.count()), TimerProc);
    return static_cast<TimerId>(id);
}

WinScheduler::TimerId WinScheduler::Every(std::chrono::milliseconds interval, std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_mutex);
    const UINT_PTR id = g_nextId++;
    g_repeating[id] = std::move(fn);
    SetTimer(nullptr, id, static_cast<UINT>(interval.count()), TimerProc);
    return static_cast<TimerId>(id);
}

void WinScheduler::Cancel(TimerId id) {
    std::lock_guard<std::mutex> lk(g_mutex);
    KillTimer(nullptr, static_cast<UINT_PTR>(id));
    g_oneShot.erase(static_cast<UINT_PTR>(id));
    g_repeating.erase(static_cast<UINT_PTR>(id));
}

void WinScheduler::Post(std::function<void()> fn) {
    After(std::chrono::milliseconds(0), std::move(fn));
}
