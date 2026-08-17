#pragma once
#include "BackButtonConfig.h"
#include "ControllerPlatform.h"
#include "TrackpadConfig.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class ControllerManager {
public:
    using StateChangedFn = std::function<void(bool connected, bool gameModeActive,
                                              bool sharedHandle, bool vigemMissing)>;
    // User-facing alert (unexpected disconnect, stall). May fire from a read
    // thread — the receiver must marshal to its own thread (e.g. PostMessage).
    using AlertFn = std::function<void(const std::wstring& title, const std::wstring& text)>;

    explicit ControllerManager(StateChangedFn onStateChanged);
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
    //
    // allowShared decides what happens when only a shared handle is available,
    // i.e. another process (in practice Steam) holds a write handle too. False
    // reports Blocked, which is what escalates to a device cycle — the only
    // thing that can invalidate the other handle and win exclusivity. True
    // settles for sharing the controller. The caller owns that policy because
    // it owns the retry budget; see TrayApp's acquire path.
    GameModeOutcome EnableGameMode(uint32_t stateWaitMs = 250, bool allowShared = false);
    void DisableGameMode();
    // Disables game mode then closes all device handles so another process
    // (e.g. Steam) can claim the controller. Safe to call when already disabled.
    void ReleaseDevices();

    // Best-effort lizard restore for crash paths — called from the unhandled-
    // exception filter installed by the constructor. Takes no locks and joins
    // no threads. The firmware's own revert timeout remains the final
    // fallback if this fails or the process dies harder than a C++ exception.
    void EmergencyRestoreAll() noexcept;

    // The whole active profile — platform, paddle bindings and both pads.
    // Mostly applied live: the read loop reads the profile every frame, so
    // bindings land on the next report. A changed platform is the exception —
    // it rebuilds the virtual controller, see the definition.
    void SetProfile(const ControllerProfile& profile);

    bool IsConnected()              const { return !m_slots.empty(); }
    bool IsGameModeActive()         const;
    // Whether any slot running game mode is doing so on a handle it shares with
    // another writer. An exclusive claim locks other processes out of the vendor
    // collection entirely; a shared one leaves Steam free to keep driving the
    // same controller, which is worth surfacing rather than hiding.
    bool IsGameModeShared()         const;
    const ControllerProfile& GetProfile() const { return m_profile; }

    // Set when a virtual pad could not be created, so the tray can say which
    // of the two very different problems it was: no bus driver at all (go
    // install one) or a bus that is present and did not work (a rebranded fork
    // is the usual reason, and telling the user to install ViGEmBus again
    // would be wrong). Both are only meaningful after a failed enable.
    bool                LastPadDriverMissing() const { return m_lastPadDriverMissing; }
    const std::wstring& LastBusReport()        const { return m_lastBusReport; }

    // Called by RemapWindow when a row enters/exits listening state.
    // Callback fires on the read thread — use PostMessage to marshal to the UI thread.
    void StartButtonCapture(std::function<void(const BackButtonBinding&)> callback);
    void StopButtonCapture();

private:
    struct Slot;

    void SyncDevices();
    void OpenSlot(const std::wstring& path);
    // recordSilent: whether a slot that fails to produce a state report should be
    // remembered as silent (and logged). Only true for a full-timeout probe —
    // see the two-pass comment in EnableGameMode.
    GameModeOutcome EnableGameModeSlot(Slot& slot, bool& padUnavailableOut, bool allowShared,
                                       uint32_t stateWaitMs, bool recordSilent);
    void DisableGameModeSlot(Slot& slot);
    void ReleaseHeldPaddleInputs(Slot& slot);
    void StartReadLoop(Slot& slot);
    void StopReadLoop(Slot& slot);
    void ReadLoop(Slot* slot);
    void NotifyStateChanged(bool padUnavailable = false);

    // Pushes the current profile's pad settings into one slot's trackpads and
    // its virtual controller. Shared by SetProfile and slot creation.
    void ApplyPadSettings(Slot& slot);

    StateChangedFn                     m_onStateChanged;
    AlertFn                            m_alertFn;
    std::vector<std::unique_ptr<Slot>> m_slots;
    bool                               m_lastPadDriverMissing = false;
    std::wstring                       m_lastBusReport;
    ControllerProfile                  m_profile;

    std::atomic<bool>                            m_capturing{false};
    std::mutex                                   m_captureMutex;
    std::function<void(const BackButtonBinding&)> m_captureCallback;
};
