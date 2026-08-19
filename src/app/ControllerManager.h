#pragma once
#include "BackButtonConfig.h"
#include "ControllerPlatform.h"
#include "TrackpadConfig.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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
    // The interface that last had a controller in it. A receiver publishes one
    // per slot and only one is ever occupied, so this is what a cycle actually
    // needs to touch — see the request file the helper reads.
    const std::wstring& LastLivePath() const { return m_lastLivePath; }
    // Where the milliseconds go in an acquire, measured rather than inferred.
    // Every timing figure in #79 so far came from reading log line order, and
    // that was wrong twice: queued WM_DEVICECHANGE messages made a five-second
    // stall look like an 80ms one. Reset at the start of each EnableGameMode.
    struct AcquireTiming {
        double enumerateMs = 0;  // SteamController::EnumerateAll, all HID devices
        double openMs      = 0;  // CreateFileW + caps queries for new slots
        double claimMs     = 0;  // the exclusive/shared claim sweep
        double probeMs     = 0;  // waiting for slots to prove they are live
        int    enumerates  = 0;  // how many full enumerations this attempt cost
    };
    const AcquireTiming& LastTiming() const { return m_timing; }
    void ResetTiming() { m_timing = {}; }

    // Win the reopen race by not waiting to be told about it.
    //
    // WM_DEVICECHANGE is one of the last events in a device arrival, not the
    // first: PnP registers the interface, then dispatches notifications to
    // every listener, and ours lands in a message queue behind whatever else
    // is pending. The device is openable well before we hear about it, and on
    // some machines Steam has it by then (#79).
    //
    // We are the ones who asked for the cycle, so we know it is coming and
    // nobody else does. BeginPounce is called just before the device is
    // released and cycled: it spins CreateFileW on the paths we already know,
    // off the UI thread, and takes the device exclusively the instant it comes
    // back. AdoptPounced then hands that live handle to a slot.
    void BeginPounce();
    void StopPounce();
    // Adopt anything the pounce thread caught. Call on the UI thread before
    // trying to acquire; returns true when at least one slot came from it.
    bool AdoptPounced();

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
    AcquireTiming                      m_timing;
    std::wstring                       m_lastLivePath;
    // Pounce state. The thread only ever touches these, never m_slots, so the
    // slot list stays single-threaded and owned by the UI thread.
    struct Pounce {
        std::thread              thread;
        std::atomic<bool>        running{false};
        std::mutex               mutex;
        std::vector<std::wstring> paths;                 // what to watch for
        std::vector<std::pair<std::wstring, void*>> caught;  // path + HANDLE
    };
    Pounce                             m_pounce;
    bool                               m_lastPadDriverMissing = false;
    std::wstring                       m_lastBusReport;
    ControllerProfile                  m_profile;

    std::atomic<bool>                            m_capturing{false};
    std::mutex                                   m_captureMutex;
    std::function<void(const BackButtonBinding&)> m_captureCallback;
};
