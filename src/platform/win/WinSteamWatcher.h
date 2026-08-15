#pragma once
// SteamState lives in the shared interface header, not declared locally here
// any more: this class predates the cross-platform port and isn't itself an
// ISteamWatcher (TrayApp still owns and calls it directly, unchanged) — but
// WinPlatform.h pulls in the real ISteamWatcher.h for its own adapter, and a
// second, differently-scoped `enum class SteamState` in the same
// translation unit would conflict with it. The values are identical either way.
#include "core/iface/ISteamWatcher.h"
#include <atomic>
#include <functional>
#include <thread>

// Watches the Steam process and its RunningAppID registry value, reporting
// debounced state transitions from a 2-second polling thread.
class SteamWatcher {
public:
    using SteamStateFn = std::function<void(SteamState)>;

    ~SteamWatcher() { Stop(); }

    // Starts the polling thread. Fires the callback once immediately with the
    // current state (so startup can assert the right mode), then on every
    // debounced transition. The callback runs on the watcher thread — marshal
    // to the UI thread (e.g. PostMessage) before touching UI or controller state.
    void Start(SteamStateFn onChange);
    void Stop();

    SteamState GetState() const { return m_state.load(); }

private:
    void PollLoop();
    static SteamState Detect();

    SteamStateFn            m_onChange;
    std::thread             m_thread;
    std::atomic<bool>       m_running{false};
    std::atomic<SteamState> m_state{SteamState::NoSteam};
};
