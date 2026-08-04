#pragma once
#include "BackButtonConfig.h"
#include "ControllerPlatform.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class ControllerManager {
public:
    using StateChangedFn = std::function<void(bool connected, bool gameModeActive, bool vigemMissing)>;
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
    enum class GameModeOutcome { Enabled, NoActiveController, Blocked };

    // stateWaitMs is how long each slot is given to prove it is live. Short
    // values are for polling a receiver whose controller is switched off —
    // a live slot streams continuously, so it answers almost immediately.
    GameModeOutcome EnableGameMode(uint32_t stateWaitMs = 250);
    void DisableGameMode();
    // Disables game mode then closes all device handles so another process
    // (e.g. Steam) can claim the controller. Safe to call when already disabled.
    void ReleaseDevices();

    // Best-effort lizard restore for crash paths — called from the unhandled-
    // exception filter installed by the constructor. Takes no locks and joins
    // no threads. The firmware's own revert timeout remains the final
    // fallback if this fails or the process dies harder than a C++ exception.
    void EmergencyRestoreAll() noexcept;

    // Whether steam.exe is currently running. Decides if game mode may settle
    // for a shared handle: a write handle held while Steam is absent belongs to
    // some benign system component, but one held while Steam is running is
    // probably Steam itself, and driving the controller alongside it is worse
    // than refusing — refusing is what escalates to a device cycle.
    void SetSteamPresent(bool present) { m_steamPresent = present; }

    void SetTrackpadMouseEnabled(bool enabled);
    void SetUseLeftTrackpad(bool enabled);
    void SetBackButtonConfig(const BackButtonConfig& cfg);
    void SetBackButtonsEnabled(bool enabled);
    void SetControllerPlatform(ControllerPlatform platform);

    bool IsConnected()              const { return !m_slots.empty(); }
    bool IsGameModeActive()         const;
    // True while some connected slot is not in game mode — an empty receiver
    // slot, or a controller that has not been switched on yet.
    bool HasInactiveSlot()          const;
    // Interfaces whose most recent attempt was refused by whoever already
    // holds them. These are the only ones a device cycle needs to touch.
    std::vector<std::wstring> BlockedSlotPaths() const;
    bool IsTrackpadMouseEnabled()   const { return m_trackpadMouseEnabled; }
    bool IsUseLeftTrackpad()        const { return m_useLeftTrackpad; }
    bool IsBackButtonsEnabled()     const { return m_backButtonsEnabled; }
    ControllerPlatform GetControllerPlatform() const { return m_controllerPlatform; }
    const BackButtonConfig& GetBackButtonConfig() const { return m_backConfig; }

    // Called by RemapWindow when a row enters/exits listening state.
    // Callback fires on the read thread — use PostMessage to marshal to the UI thread.
    void StartButtonCapture(std::function<void(BackButtonAction)> callback);
    void StopButtonCapture();

private:
    struct Slot;

    void SyncDevices();
    void OpenSlot(const std::wstring& path);
    GameModeOutcome EnableGameModeSlot(Slot& slot, bool& vigemMissingOut,
                                       uint32_t stateWaitMs);
    void DisableGameModeSlot(Slot& slot);
    void StartReadLoop(Slot& slot);
    void StopReadLoop(Slot& slot);
    void ReadLoop(Slot* slot);
    void NotifyStateChanged(bool vigemMissing = false);

    StateChangedFn                     m_onStateChanged;
    AlertFn                            m_alertFn;
    std::vector<std::unique_ptr<Slot>> m_slots;
    bool                               m_trackpadMouseEnabled = false;
    bool                               m_useLeftTrackpad      = false;
    bool                               m_backButtonsEnabled   = false;
    bool                               m_steamPresent         = false;
    ControllerPlatform                 m_controllerPlatform   = ControllerPlatform::Xbox;
    BackButtonConfig                   m_backConfig;

    std::atomic<bool>                        m_capturing{false};
    std::mutex                               m_captureMutex;
    std::function<void(BackButtonAction)>    m_captureCallback;
};
