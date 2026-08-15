#pragma once
#include "core/iface/ISteamWatcher.h"
#include <atomic>
#include <thread>

// Polls ~/.steam/registry.vdf's SteamPID (Steam writes its own pid there,
// "0" when not running) and /proc/*/environ's SteamAppId (Steam exports it
// into every game process it launches, native or Proton) to answer the same
// question SteamWatcher answers on Windows from the registry. Same
// debounce/hysteresis policy as the Windows implementation — see
// SteamState's ordering comment.
class LinuxSteamWatcher : public ISteamWatcher {
public:
    ~LinuxSteamWatcher() override { Stop(); }

    void Start(SteamStateFn onChange) override;
    void Stop() override;
    SteamState GetState() const override { return m_state.load(); }

private:
    void PollLoop();
    static SteamState Detect();

    SteamStateFn            m_onChange;
    std::thread             m_thread;
    std::atomic<bool>       m_running{false};
    std::atomic<SteamState> m_state{SteamState::NoSteam};
};
