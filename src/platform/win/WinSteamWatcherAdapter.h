#pragma once
#include "core/iface/ISteamWatcher.h"
#include "WinSteamWatcher.h"

// Delegates to a plain SteamWatcher instance of its own, so WinPlatform has
// a real ISteamWatcher to hand back. Not the same instance TrayApp owns and
// polls directly (TrayApp's SteamWatcher m_steamWatcher member is unchanged
// from before the port) — ControllerManager never calls IPlatform::Steam(),
// so this one is simply never exercised. Two independent pollers is mildly
// redundant, not incorrect: both just watch the same OS-level facts.
class WinSteamWatcherAdapter : public ISteamWatcher {
public:
    void Start(SteamStateFn onChange) override { m_watcher.Start(std::move(onChange)); }
    void Stop() override { m_watcher.Stop(); }
    SteamState GetState() const override { return m_watcher.GetState(); }

private:
    SteamWatcher m_watcher;
};
