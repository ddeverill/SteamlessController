#pragma once
#include "core/iface/IForegroundWatcher.h"

// Wayland deliberately has no standard cross-compositor "what window is
// focused" API, so there is no one mechanism this class can use the way
// SetWinEventHook works everywhere on Windows. A real implementation is
// compositor-specific:
//   - KDE/KWin exposes org_kde_plasma_window_management, which gives
//     title/app_id/activated state and (uniquely among the options) pid;
//   - wlroots compositors (Sway, Hyprland, ...) expose
//     zwlr_foreign_toplevel_management_unstable_v1 (title/app_id/activated,
//     no pid);
//   - GNOME/Mutter has no public protocol or D-Bus API at all — a companion
//     GNOME Shell extension would be required.
//
// Wiring up a Wayland protocol client is a substantial follow-on piece of
// work (linking libwayland-client, generating bindings from the protocol
// XML) that this port defers rather than ships half-verified. In the
// meantime this class is the honest placeholder the architecture calls
// for: it reports Unavailable rather than guessing, so ControllerService
// and `steamlessctl doctor` can say plainly that per-game profile
// auto-switching needs `profile activate` on this system, instead of
// silently never firing.
class LinuxForegroundWatcher : public IForegroundWatcher {
public:
    Support Availability() const override { return Support::Unavailable; }
    bool Start(std::function<void(const ForegroundIdentity&)>) override { return false; }
    void Stop() override {}
    ForegroundIdentity Current() const override { return {}; }
};
