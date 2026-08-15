#pragma once
#include <functional>
#include <string>

// Which app is currently focused, for per-game profile matching. Fields are
// filled in as far as the backend can manage — see Availability() below —
// and an empty struct always means "unknown", never "the desktop".
struct ForegroundIdentity {
    std::string exePath;     // resolved binary path, when known
    std::string appId;       // Windows: AUMID. Linux: .desktop id / wl app_id
    std::string title;
    std::string steamAppId;  // populated when the process can be tied to a Steam app id
    long        pid = 0;

    bool Empty() const { return exePath.empty() && appId.empty() && pid == 0; }

    // Exact (case-sensitive) comparison — right for Linux's case-sensitive
    // paths. A Windows backend that wants case-insensitive matching folds
    // case itself before comparing, since Windows is the one platform where
    // two spellings of a path can name the same file.
    bool operator==(const ForegroundIdentity& o) const {
        return exePath == o.exePath && appId == o.appId && pid == o.pid;
    }
    bool operator!=(const ForegroundIdentity& o) const { return !(*this == o); }
};

class IForegroundWatcher {
public:
    // How much this backend's answers can be trusted. Wayland has no
    // standard cross-compositor "what's focused" API, so a Linux backend may
    // legitimately be able to do nothing — Unavailable is an honest answer,
    // not a bug. ControllerService and `steamlessctl doctor` surface this
    // rather than silently never firing per-game profile switches.
    enum class Support { Reliable, BestEffort, Unavailable };

    virtual ~IForegroundWatcher() = default;

    virtual Support Availability() const = 0;
    virtual bool Start(std::function<void(const ForegroundIdentity&)> onChanged) = 0;
    virtual void Stop() = 0;
    virtual ForegroundIdentity Current() const = 0;
};
