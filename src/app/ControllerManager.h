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

    explicit ControllerManager(StateChangedFn onStateChanged);
    ~ControllerManager();
    ControllerManager(const ControllerManager&) = delete;
    ControllerManager& operator=(const ControllerManager&) = delete;

    void OnDeviceChange();

    void EnableGameMode();
    void DisableGameMode();
    // Disables game mode then closes all device handles so another process
    // (e.g. Steam) can claim the controller. Safe to call when already disabled.
    void ReleaseDevices();

    void SetTrackpadMouseEnabled(bool enabled);
    void SetUseLeftTrackpad(bool enabled);
    void SetBackButtonConfig(const BackButtonConfig& cfg);
    void SetBackButtonsEnabled(bool enabled);
    void SetControllerPlatform(ControllerPlatform platform);

    bool IsConnected()              const { return !m_slots.empty(); }
    bool IsGameModeActive()         const;
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
    void EnableGameModeSlot(Slot& slot, bool& vigemMissingOut);
    void DisableGameModeSlot(Slot& slot);
    void StartReadLoop(Slot& slot);
    void StopReadLoop(Slot& slot);
    void ReadLoop(Slot* slot);
    void NotifyStateChanged(bool vigemMissing = false);

    StateChangedFn                     m_onStateChanged;
    std::vector<std::unique_ptr<Slot>> m_slots;
    bool                               m_trackpadMouseEnabled = false;
    bool                               m_useLeftTrackpad      = false;
    bool                               m_backButtonsEnabled   = false;
    ControllerPlatform                 m_controllerPlatform   = ControllerPlatform::Xbox;
    BackButtonConfig                   m_backConfig;

    std::atomic<bool>                        m_capturing{false};
    std::mutex                               m_captureMutex;
    std::function<void(BackButtonAction)>    m_captureCallback;
};
