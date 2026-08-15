#include "LinuxScheduler.h"

LinuxScheduler::TimerId LinuxScheduler::After(std::chrono::milliseconds delay, std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(m_mutex);
    const TimerId id = m_nextId++;
    m_timers[id] = Timer{ std::chrono::steady_clock::now() + delay, std::chrono::milliseconds(0), std::move(fn), false };
    return id;
}

LinuxScheduler::TimerId LinuxScheduler::Every(std::chrono::milliseconds interval, std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(m_mutex);
    const TimerId id = m_nextId++;
    m_timers[id] = Timer{ std::chrono::steady_clock::now() + interval, interval, std::move(fn), false };
    return id;
}

void LinuxScheduler::Cancel(TimerId id) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_timers.find(id);
    if (it != m_timers.end()) it->second.cancelled = true;
}

void LinuxScheduler::Post(std::function<void()> fn) {
    After(std::chrono::milliseconds(0), std::move(fn));
}

int LinuxScheduler::RunDuePending() {
    for (;;) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            // Drop anything cancelled first so it can't be picked as "next".
            for (auto it = m_timers.begin(); it != m_timers.end();) {
                if (it->second.cancelled) it = m_timers.erase(it);
                else ++it;
            }

            auto next = m_timers.end();
            for (auto it = m_timers.begin(); it != m_timers.end(); ++it)
                if (next == m_timers.end() || it->second.due < next->second.due) next = it;

            if (next == m_timers.end()) return -1;  // nothing pending

            const auto now = std::chrono::steady_clock::now();
            if (next->second.due > now) {
                return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    next->second.due - now).count());
            }

            fn = next->second.fn;
            if (next->second.interval.count() > 0) next->second.due += next->second.interval;
            else m_timers.erase(next);
        }
        // Run without the lock held — the callback may itself call
        // After/Every/Post/Cancel, which would deadlock re-entering here.
        if (fn) fn();
    }
}
