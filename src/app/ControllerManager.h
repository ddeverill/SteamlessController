#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ControllerManager {
public:
    using StateChangedFn = std::function<void(bool connected, bool gameModeActive, bool vigemMissing)>;

    explicit ControllerManager(StateChangedFn onStateChanged);
    ~ControllerManager();
    ControllerManager(const ControllerManager&) = delete;
    ControllerManager& operator=(const ControllerManager&) = delete;

    // Called when Windows reports a device arrival or removal (WM_DEVICECHANGE).
    void OnDeviceChange();

    // Toggle game mode on/off. Applied to all connected controllers.
    void EnableGameMode();
    void DisableGameMode();

    void SetTrackpadMouseEnabled(bool enabled);
    void SetBackButtonsEnabled(bool enabled);
    void SetUseLeftTrackpad(bool enabled);

    bool IsConnected()             const { return !m_slots.empty(); }
    bool IsGameModeActive()        const;
    bool IsTrackpadMouseEnabled()  const { return m_trackpadMouseEnabled; }
    bool IsBackButtonsEnabled()    const { return m_backButtonsEnabled; }
    bool IsUseLeftTrackpad()       const { return m_useLeftTrackpad; }

private:
    struct Slot;

    // Enumerates live devices, adds new slots, removes disconnected ones.
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
    bool                               m_backButtonsEnabled   = false;
    bool                               m_useLeftTrackpad      = false;
};
