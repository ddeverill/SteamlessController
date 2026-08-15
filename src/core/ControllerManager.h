#pragma once
#include "BackButtonConfig.h"
#include "ControllerPlatform.h"
#include "ModifierTracker.h"
#include "TrackpadConfig.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class IPlatform;

class ControllerManager {
public:
    using StateChangedFn = std::function<void(bool connected, bool gameModeActive, bool vigemMissing)>;
    // User-facing alert (unexpected disconnect, stall). May fire from a read
    // thread — the receiver must marshal to its own thread (e.g. via
    // IScheduler::Post).
    using AlertFn = std::function<void(const std::string& title, const std::string& text)>;

    ControllerManager(IPlatform& platform, StateChangedFn onStateChanged);
    ~ControllerManager();
    ControllerManager(const ControllerManager&) = delete;
    ControllerManager& operator=(const ControllerManager&) = delete;

    void OnDeviceChange();

    // Set once right after construction, before any game mode is enabled —
    // read loops copy it without locking.
    void SetAlertCallback(AlertFn fn) { m_alertFn = std::move(fn); }

    // Why game mode did not come up, so callers can tell an idle receiver from
    // a contested one. A puck publishes a slot interface whether or not a
    // controller is paired into it, and cycling the device because nothing is
    // switched on is pointless churn.
    //
    // VirtualPadUnavailable is emphatically not Blocked: nothing is wrong with
    // the controller, so retrying fast or restarting its devnode accomplishes
    // nothing. Reporting it as Blocked is what pointed the device-cycling
    // machinery at a reporter's puck over a missing bus driver and left its
    // HID collections disabled (#65).
    enum class GameModeOutcome { Enabled, NoActiveController, Blocked, VirtualPadUnavailable };

    // stateWaitMs is how long each slot is given to prove it is live. Short
    // values are for polling a receiver whose controller is switched off —
    // a live slot streams continuously, so it answers almost immediately.
    GameModeOutcome EnableGameMode(uint32_t stateWaitMs = 250);
    void DisableGameMode();
    // Disables game mode then closes all device handles so another process
    // (e.g. Steam) can claim the controller. Safe to call when already disabled.
    void ReleaseDevices();

    // Best-effort lizard restore for crash paths — called from the crash
    // handler installed by the constructor (IPlatform::InstallCrashHandler).
    // Takes no locks and joins no threads. The firmware's own revert timeout
    // remains the final fallback if this fails or the process dies harder
    // than this handler can react to.
    void EmergencyRestoreAll() noexcept;

    // Whether Steam is currently running. Decides if game mode may settle
    // for a shared handle: a write handle held while Steam is absent belongs to
    // some benign system component, but one held while Steam is running is
    // probably Steam itself, and driving the controller alongside it is worse
    // than refusing — refusing is what escalates to a device cycle.
    void SetSteamPresent(bool present) { m_steamPresent = present; }

    // The whole active profile — platform, paddle bindings and both pads.
    // Mostly applied live: the read loop reads the profile every frame, so
    // bindings land on the next report. A changed platform is the exception —
    // it rebuilds the virtual controller, see the definition.
    void SetProfile(const ControllerProfile& profile);

    bool IsConnected()              const { return !m_slots.empty(); }
    bool IsGameModeActive()         const;
    const ControllerProfile& GetProfile() const { return m_profile; }

    // Set when a virtual pad could not be created, so callers can say which
    // of the two very different problems it was: no bus/driver at all (go
    // install/enable one) or a bus that is present and did not work. Both are
    // only meaningful after a failed enable.
    bool               LastPadDriverMissing() const { return m_lastPadDriverMissing; }
    const std::string& LastBusReport()        const { return m_lastBusReport; }

    // Called by the remap UI/CLI when a row enters/exits listening state.
    // Callback fires on the read thread — marshal to the owning thread
    // (e.g. via IScheduler::Post) before touching UI state from it.
    void StartButtonCapture(std::function<void(const BackButtonBinding&)> callback);
    void StopButtonCapture();

private:
    struct Slot;

    void SyncDevices();
    void OpenSlot(const struct HidDeviceInfo& info);
    GameModeOutcome EnableGameModeSlot(Slot& slot, bool& padUnavailableOut,
                                       uint32_t stateWaitMs);
    void DisableGameModeSlot(Slot& slot);
    void ReleaseHeldPaddleInputs(Slot& slot);
    void StartReadLoop(Slot& slot);
    void StopReadLoop(Slot& slot);
    void ReadLoop(Slot* slot);
    void NotifyStateChanged(bool padUnavailable = false);

    // Sends the key/mouse/touch-keyboard event a binding stands for. Gamepad
    // actions reach the virtual pad instead and are ignored here. Returns
    // whether anything was sent.
    bool SendPaddleInput(const BackButtonBinding& binding, bool down);

    // Pushes the current profile's pad settings into one slot's trackpads and
    // its virtual controller. Shared by SetProfile and slot creation.
    void ApplyPadSettings(Slot& slot);

    IPlatform&                         m_platform;
    ModifierTracker                    m_modifiers;
    StateChangedFn                     m_onStateChanged;
    AlertFn                            m_alertFn;
    std::vector<std::unique_ptr<Slot>> m_slots;
    bool                               m_steamPresent         = false;
    bool                               m_lastPadDriverMissing = false;
    std::string                        m_lastBusReport;
    ControllerProfile                  m_profile;

    std::atomic<bool>                            m_capturing{false};
    std::mutex                                   m_captureMutex;
    std::function<void(const BackButtonBinding&)> m_captureCallback;
};
