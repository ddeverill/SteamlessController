#include "ControllerManager.h"
#include "VirtualController.h"
#include "TrackpadMouse.h"
#include "steam/SteamController.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>

// ---------------------------------------------------------------------------
// Slot — owns one physical controller and its virtual counterpart.
// All access is from the UI thread except ReadLoop, which only touches
// its own slot's sc/vc/trackpad while readRunning is true.
// ---------------------------------------------------------------------------

struct ControllerManager::Slot {
    std::wstring                       path;
    std::unique_ptr<SteamController>   sc;
    std::unique_ptr<VirtualController> vc;
    TrackpadMouse                      trackpad;
    std::thread                        readThread;
    std::atomic<bool>                  readRunning{false};
    bool                               gameModeActive = false;

    Slot() = default;
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

ControllerManager::ControllerManager(StateChangedFn onStateChanged)
    : m_onStateChanged(std::move(onStateChanged))
{
    SyncDevices();
}

ControllerManager::~ControllerManager() {
    for (auto& slot : m_slots)
        DisableGameModeSlot(*slot);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool ControllerManager::IsGameModeActive() const {
    return std::any_of(m_slots.begin(), m_slots.end(),
        [](const auto& s) { return s->gameModeActive; });
}

void ControllerManager::OnDeviceChange() {
    SyncDevices();
}

void ControllerManager::EnableGameMode() {
    bool anyVigemMissing = false;
    for (auto& slot : m_slots)
        EnableGameModeSlot(*slot, anyVigemMissing);
    NotifyStateChanged(anyVigemMissing);
}

void ControllerManager::DisableGameMode() {
    for (auto& slot : m_slots)
        DisableGameModeSlot(*slot);
    NotifyStateChanged();
}

void ControllerManager::SetTrackpadMouseEnabled(bool enabled) {
    m_trackpadMouseEnabled = enabled;
    for (auto& slot : m_slots)
        slot->trackpad.SetTrackpadEnabled(enabled);
}

void ControllerManager::SetBackButtonsEnabled(bool enabled) {
    m_backButtonsEnabled = enabled;
    for (auto& slot : m_slots)
        slot->trackpad.SetBackButtonsEnabled(enabled);
}

void ControllerManager::SetUseLeftTrackpad(bool enabled) {
    m_useLeftTrackpad = enabled;
    for (auto& slot : m_slots)
        slot->trackpad.SetUseLeftTrackpad(enabled);
}

// ---------------------------------------------------------------------------
// Device management
// ---------------------------------------------------------------------------

void ControllerManager::SyncDevices() {
    auto livePaths = SteamController::EnumerateAll();

    // Remove slots whose device is no longer live.
    auto it = m_slots.begin();
    while (it != m_slots.end()) {
        bool alive = std::any_of(livePaths.begin(), livePaths.end(),
            [&](const auto& p) { return p == (*it)->path; });
        if (!alive) {
            DisableGameModeSlot(**it);
            it = m_slots.erase(it);
        } else {
            ++it;
        }
    }

    // Add slots for newly connected devices.
    for (auto const& path : livePaths) {
        bool already = std::any_of(m_slots.begin(), m_slots.end(),
            [&](const auto& s) { return s->path == path; });
        if (!already)
            OpenSlot(path);
    }

    NotifyStateChanged();
}

void ControllerManager::OpenSlot(const std::wstring& path) {
    auto slot = std::make_unique<Slot>();
    slot->path = path;
    slot->sc   = std::make_unique<SteamController>();
    if (!slot->sc->Open(path)) return;

    m_slots.push_back(std::move(slot));

    // If game mode is already active, bring the new controller in immediately.
    if (IsGameModeActive()) {
        bool dummy = false;
        EnableGameModeSlot(*m_slots.back(), dummy);
    }
}

// ---------------------------------------------------------------------------
// Game mode per-slot helpers
// ---------------------------------------------------------------------------

void ControllerManager::EnableGameModeSlot(Slot& slot, bool& vigemMissingOut) {
    if (slot.gameModeActive) return;
    if (!slot.sc->DisableLizardMode()) return;

    slot.vc = std::make_unique<VirtualController>();
    if (!slot.vc->IsValid()) {
        if (slot.vc->IsDriverMissing()) vigemMissingOut = true;
        slot.vc.reset();
        slot.sc->EnableLizardMode();
        return;
    }

    slot.gameModeActive = true;
    slot.trackpad.Reset();
    slot.trackpad.SetTrackpadEnabled(m_trackpadMouseEnabled);
    slot.trackpad.SetBackButtonsEnabled(m_backButtonsEnabled);
    slot.trackpad.SetUseLeftTrackpad(m_useLeftTrackpad);
    StartReadLoop(slot);
}

void ControllerManager::DisableGameModeSlot(Slot& slot) {
    if (!slot.gameModeActive) return;
    StopReadLoop(slot);
    slot.trackpad.Reset();
    slot.vc.reset();
    slot.sc->EnableLizardMode();
    slot.gameModeActive = false;
}

// ---------------------------------------------------------------------------
// Read loop
// ---------------------------------------------------------------------------

void ControllerManager::StartReadLoop(Slot& slot) {
    slot.readRunning = true;
    slot.readThread  = std::thread(&ControllerManager::ReadLoop, this, &slot);
}

void ControllerManager::StopReadLoop(Slot& slot) {
    slot.readRunning = false;
    if (slot.readThread.joinable())
        slot.readThread.join();
}

void ControllerManager::ReadLoop(Slot* slot) {
    uint8_t buf[64];
    while (slot->readRunning) {
        size_t n = slot->sc->ReadReport(buf, sizeof(buf), /*timeoutMs=*/32);
        if (n == 0) continue;
        if (buf[0] != SteamController::REPORT_STATE) continue;
        if (slot->vc) slot->vc->Update(buf, n);
        slot->trackpad.Update(buf, n);
    }
}

void ControllerManager::NotifyStateChanged(bool vigemMissing) {
    m_onStateChanged(!m_slots.empty(), IsGameModeActive(), vigemMissing);
}
