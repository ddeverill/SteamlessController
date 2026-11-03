#pragma once
#include <Windows.h>
#include <functional>
#include <memory>

// The tray icon's popup: a small custom-drawn panel in place of a native
// TrackPopupMenu. It owns nothing but its own appearance — what it shows comes
// from a Model the caller supplies, and what the user does comes back out as an
// Action. TrayApp maps those onto the same commands the old menu sent, so the
// behaviour behind each control did not move.
//
// Modeless: Show returns at once and the panel lives until it is dismissed
// (click-away, Esc, or an action that leaves the panel behind). While open it
// re-asks the provider a couple of times a second, so the status line and
// battery level stay true without the caller pushing anything.
class TrayPanel {
public:
    enum class Action {
        ToggleControl,
        ModeManual, ModeSteam, ModeGame, ModeProfile,
        Customize,
        Startup, EnableOnLaunch, Notifications,
        OpenLog, Exit,
        EnableDevice,
    };

    struct Model {
        // The devnode is switched off in Windows. Nothing else on the panel can
        // work until it is back, so this puts the way out above everything.
        bool deviceDisabled = false;
        bool connected      = false;
        bool manual         = true;   // the toggle is only the user's in Manual
        bool controlOn      = false;  // the toggle's state: intent in Manual, the
                                      // actual state under an auto mode
        int  mode           = 0;      // AutoMode's value: 0 Manual .. 3 Profile
        int  battery        = -1;     // percent, or -1 when there is no reading
        bool startup        = false;
        bool enableOnLaunch = false;  // only drawn in Manual mode
        bool notifications  = false;

        bool operator==(const Model&) const = default;
    };

    using ModelProvider = std::function<Model()>;
    using ActionHandler = std::function<void(Action)>;

    TrayPanel();
    ~TrayPanel();
    TrayPanel(const TrayPanel&) = delete;
    TrayPanel& operator=(const TrayPanel&) = delete;

    // Opens the panel with its bottom-right corner at `anchor` (the cursor, when
    // the tray icon was clicked), nudged to stay inside the monitor's work area.
    // Replaces a panel that is already open.
    void Show(HINSTANCE hInst, POINT anchor, ModelProvider provider, ActionHandler handler);

    void Close();
    bool IsOpen() const;

    // True for a moment after the panel was dismissed by clicking elsewhere.
    // Clicking the tray icon to close it deactivates the panel on button-down
    // and delivers the tray message on button-up, so without this the same
    // click would close the panel and then open it again.
    bool RecentlyDismissed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_d;
};
