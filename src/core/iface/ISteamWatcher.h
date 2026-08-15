#pragma once
#include <functional>

// What Steam is doing right now, as far as controller ownership is
// concerned. Ordered by "how much Steam is going on" — transitions to a
// HIGHER value are reported immediately (yield the controller fast),
// transitions to a LOWER value are debounced (don't grab it during a Steam
// restart or game relaunch).
enum class SteamState {
    NoSteam   = 0,  // Steam not running
    SteamIdle = 1,  // Steam running, no game active
    InGame    = 2,  // Steam running a game
};

class ISteamWatcher {
public:
    using SteamStateFn = std::function<void(SteamState)>;

    virtual ~ISteamWatcher() = default;

    // Starts polling. Fires the callback once immediately with the current
    // state, then on every debounced transition. The callback may run on a
    // background thread — marshal via IScheduler::Post before touching
    // controller state from it.
    virtual void Start(SteamStateFn onChange) = 0;
    virtual void Stop() = 0;
    virtual SteamState GetState() const = 0;
};
