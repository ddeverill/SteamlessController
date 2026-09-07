#pragma once
#include <Windows.h>
#include <string>

// Is the game a profile was matched against still running?
//
// The foreground is the only push-based signal this app has for "what is the
// user doing" (see ForegroundWatcher), and it answers a different question
// from the one profiles are really asking. Alt-tabbing to a browser during a
// matchmaking queue makes the foreground say "browser" while the honest answer
// to "what am I playing" is still the game. Asking whether the game's process
// is alive is what tells the two apart.
//
// This exists for exactly one decision: which kind of virtual pad to present.
// A profile that changes platform tears the pad down and builds it again,
// which a running game sees as its controller being unplugged, so that must
// not happen every time somebody alt-tabs. Bindings stay tied to the
// foreground, because they describe what the user is doing right now — and
// when the desktop is in front, the desktop's bindings are what is wanted.
//
// Liveness is deliberately a poor man's check rather than a process watcher.
// It is asked a few times a second at most, and it never needs to be right the
// instant a process dies — nothing is watching the pad in the moment a game
// exits, so noticing a second or two late costs nothing.
class GameLiveness {
public:
    GameLiveness() = default;
    ~GameLiveness() { Clear(); }
    GameLiveness(const GameLiveness&) = delete;
    GameLiveness& operator=(const GameLiveness&) = delete;

    // Start holding for this profile and process. Replaces whatever was held.
    void Hold(const std::wstring& profileId, DWORD pid);

    // Stop holding, whatever the reason.
    void Clear();

    bool               IsHeld()    const { return !m_profileId.empty(); }
    const std::wstring& ProfileId() const { return m_profileId; }
    // The process the hold is against, so a caller can notice the same game
    // running under a new one — a relaunch inside the poll interval would
    // otherwise leave the hold pinned to a pid that has already gone.
    DWORD               Pid()       const { return m_pid; }

    // Whether the held process is still alive. False when nothing is held.
    //
    // Anti-cheat and elevated games routinely refuse an unelevated process the
    // SYNCHRONIZE right, so a handle is not always available and its absence
    // says nothing about whether the game is running. The fallback re-opens by
    // pid and asks for the exit code, because opening it only proves the
    // process OBJECT exists: a pid stays valid for as long as anything holds a
    // handle to the process, which a launcher or an anti-cheat service
    // generally does. Where it cannot be opened at all, being DENIED access is
    // itself proof the process is there.
    //
    // The fallback can be fooled two ways, both benign. A game exiting with
    // code 259 reads as STILL_ACTIVE forever, and pid reuse needs the id to be
    // handed to something new between two checks seconds apart. Either leaves a
    // hold standing too long, costing the pad rebuild it was deferring — which
    // is the behaviour this class was added to improve on, not worse than it.
    bool StillRunning() const;

private:
    std::wstring m_profileId;
    DWORD        m_pid    = 0;
    HANDLE       m_handle = nullptr;  // null when the process refused us one
};
