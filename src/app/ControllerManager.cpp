#include "ControllerManager.h"
#include "VirtualController.h"
#include "TrackpadMouse.h"
#include "steam/SteamController.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <thread>

// ---------------------------------------------------------------------------
// Slot
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
// Rising-edge capture: returns true (and sets 'out') when any of the 16
// mappable XInput buttons transitions from not-pressed to pressed.
// Back paddles (L4/L5/R4/R5) are excluded — they are what we are remapping,
// not valid capture targets.
// ---------------------------------------------------------------------------

static bool DetectCapture(const uint8_t* cur, const uint8_t* prev,
                          BackButtonAction& out)
{
    using BA = BackButtonAction;

    // Helper: rising edge on a single bit in a byte.
    auto rose = [&](int byteIdx, uint8_t mask) -> bool {
        return (cur[byteIdx] & mask) && !(prev[byteIdx] & mask);
    };

    // Triggers use 16-bit analog — fire on crossing 25% (0x2000).
    auto trigRose = [&](int byteIdx) -> bool {
        int16_t c, p;
        memcpy(&c, cur  + byteIdx, 2);
        memcpy(&p, prev + byteIdx, 2);
        return c > 0x2000 && p <= 0x2000;
    };

    // Check each mappable button in priority order.
    // Byte offsets match SteamController.h for the 0x45 report.
    if (rose(2, SteamController::BTN_A))        { out = BA::A;         return true; }
    if (rose(2, SteamController::BTN_B))        { out = BA::B;         return true; }
    if (rose(2, SteamController::BTN_X))        { out = BA::X;         return true; }
    if (rose(2, SteamController::BTN_Y))        { out = BA::Y;         return true; }
    if (rose(4, SteamController::BTN_LB))       { out = BA::LB;        return true; }
    if (rose(3, SteamController::BTN_RB))       { out = BA::RB;        return true; }
    if (trigRose(6))                            { out = BA::LT;        return true; }
    if (trigRose(8))                            { out = BA::RT;        return true; }
    if (rose(3, SteamController::BTN_DPAD_UP))  { out = BA::DPadUp;    return true; }
    if (rose(3, SteamController::BTN_DPAD_DN))  { out = BA::DPadDown;  return true; }
    if (rose(3, SteamController::BTN_DPAD_LT))  { out = BA::DPadLeft;  return true; }
    if (rose(3, SteamController::BTN_DPAD_RT))  { out = BA::DPadRight; return true; }
    if (rose(2, SteamController::BTN_MENU))     { out = BA::Menu;      return true; }
    if (rose(3, SteamController::BTN_VIEW))     { out = BA::View;      return true; }
    if (rose(3, SteamController::BTN_LS))       { out = BA::L3;        return true; }
    if (rose(2, SteamController::BTN_RS))       { out = BA::R3;        return true; }

    return false;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

ControllerManager::ControllerManager(StateChangedFn onStateChanged)
    : m_onStateChanged(std::move(onStateChanged))
{
    SyncDevices();
}

ControllerManager::~ControllerManager() {
    for (auto& slot : m_slots) {
        // Stop the read loop and virtual controller first (same as DisableGameModeSlot
        // but without the gameModeActive guard — we always want to restore lizard mode).
        if (slot->gameModeActive) {
            StopReadLoop(*slot);
            slot->trackpad.Reset();
            slot->vc.reset();
            slot->gameModeActive = false;
        }
        slot->sc->EnableLizardMode(); // always restore, even if game mode was never active
    }
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

void ControllerManager::SetUseLeftTrackpad(bool enabled) {
    m_useLeftTrackpad = enabled;
    for (auto& slot : m_slots)
        slot->trackpad.SetUseLeftTrackpad(enabled);
}

void ControllerManager::SetBackButtonConfig(const BackButtonConfig& cfg) {
    m_backConfig = cfg;
    // Live update: the ReadLoop reads m_backConfig on every frame, so the
    // change takes effect on the very next report.
}

void ControllerManager::SetBackButtonsEnabled(bool enabled) {
    m_backButtonsEnabled = enabled;
    for (auto& slot : m_slots)
        slot->trackpad.SetBackButtonsEnabled(enabled);
}

void ControllerManager::SetControllerPlatform(ControllerPlatform platform) {
    if (m_controllerPlatform == platform) return;
    m_controllerPlatform = platform;
    if (IsGameModeActive()) {
        DisableGameMode();
        EnableGameMode();
    }
}

void ControllerManager::StartButtonCapture(std::function<void(BackButtonAction)> callback) {
    std::lock_guard<std::mutex> lk(m_captureMutex);
    m_captureCallback = std::move(callback);
    m_capturing = true;
}

void ControllerManager::StopButtonCapture() {
    m_capturing = false;
    std::lock_guard<std::mutex> lk(m_captureMutex);
    m_captureCallback = nullptr;
}

// ---------------------------------------------------------------------------
// Device management
// ---------------------------------------------------------------------------

void ControllerManager::SyncDevices() {
    auto livePaths = SteamController::EnumerateAll();

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

    if (IsGameModeActive()) {
        bool dummy = false;
        EnableGameModeSlot(*m_slots.back(), dummy);
    } else {
        // Restore lizard mode in case a previous session crashed without cleaning up.
        m_slots.back()->sc->EnableLizardMode();
    }
}

// ---------------------------------------------------------------------------
// Game mode per-slot helpers
// ---------------------------------------------------------------------------

void ControllerManager::EnableGameModeSlot(Slot& slot, bool& vigemMissingOut) {
    if (slot.gameModeActive) return;
    if (!slot.sc->DisableLizardMode()) return;

    slot.vc = std::make_unique<VirtualController>(
        m_controllerPlatform,
        [sc = slot.sc.get()](uint8_t largeMotor, uint8_t smallMotor) {
            if (!sc->IsOpen()) return;
            const uint16_t leftSpeed  = static_cast<uint16_t>(static_cast<uint32_t>(largeMotor) * 257u);
            const uint16_t rightSpeed = static_cast<uint16_t>(static_cast<uint32_t>(smallMotor) * 257u);
            sc->SetRumble(leftSpeed, rightSpeed);
        });
    if (!slot.vc->IsValid()) {
        if (slot.vc->IsDriverMissing()) vigemMissingOut = true;
        slot.vc.reset();
        slot.sc->EnableLizardMode();
        return;
    }

    slot.gameModeActive = true;
    slot.trackpad.Reset();
    slot.trackpad.SetTrackpadEnabled(m_trackpadMouseEnabled);
    slot.trackpad.SetUseLeftTrackpad(m_useLeftTrackpad);
    slot.trackpad.SetBackButtonsEnabled(m_backButtonsEnabled);
    StartReadLoop(slot);
}

void ControllerManager::DisableGameModeSlot(Slot& slot) {
    if (!slot.gameModeActive) return;
    StopReadLoop(slot);
    if (slot.sc->IsOpen())
        slot.sc->SetRumble(0, 0);
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
    uint8_t prevBuf[64] = {};
    bool    hasPrev     = false;

    while (slot->readRunning) {
        size_t n = slot->sc->ReadReport(buf, sizeof(buf), /*timeoutMs=*/32);
        if (n == 0) continue;
        if (buf[0] != SteamController::REPORT_STATE) continue;

        if (slot->vc) slot->vc->Update(buf, n, m_backConfig, m_backButtonsEnabled);
        slot->trackpad.Update(buf, n);

        // Fire mouse button events for back paddles mapped to LeftMouseButton/RightMouseButton.
        // Uses edge detection so we only send DOWN on press and UP on release.
        // Skipped when backButtonsEnabled — TrackpadMouse already handles L4/R4 in that mode.
        if (!m_backButtonsEnabled && hasPrev) {
            struct PaddleEdge { bool cur; bool prev; BackButtonAction action; };
            const PaddleEdge edges[] = {
                { n>4 && (buf[4]&SteamController::BTN_L4)!=0, (prevBuf[4]&SteamController::BTN_L4)!=0, m_backConfig.l4 },
                { n>4 && (buf[4]&SteamController::BTN_L5)!=0, (prevBuf[4]&SteamController::BTN_L5)!=0, m_backConfig.l5 },
                { n>2 && (buf[2]&SteamController::BTN_R4)!=0, (prevBuf[2]&SteamController::BTN_R4)!=0, m_backConfig.r4 },
                { n>3 && (buf[3]&SteamController::BTN_R5)!=0, (prevBuf[3]&SteamController::BTN_R5)!=0, m_backConfig.r5 },
            };
            for (const auto& e : edges) {
                if (e.cur == e.prev) continue;
                DWORD flag = 0;
                if      (e.action == BackButtonAction::LeftMouseButton)  flag = e.cur ? MOUSEEVENTF_LEFTDOWN  : MOUSEEVENTF_LEFTUP;
                else if (e.action == BackButtonAction::RightMouseButton) flag = e.cur ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
                if (flag) { INPUT inp{}; inp.type = INPUT_MOUSE; inp.mi.dwFlags = flag; SendInput(1, &inp, sizeof(INPUT)); }
            }
        }

        // Button capture for the remap window (press-to-bind).
        // Only fires on a rising edge to avoid repeat-triggering on hold.
        if (m_capturing.load() && hasPrev) {
            BackButtonAction captured;
            if (DetectCapture(buf, prevBuf, captured)) {
                m_capturing = false;
                std::lock_guard<std::mutex> lk(m_captureMutex);
                if (m_captureCallback) m_captureCallback(captured);
            }
        }

        memcpy(prevBuf, buf, n);
        hasPrev = true;
    }
}

void ControllerManager::NotifyStateChanged(bool vigemMissing) {
    m_onStateChanged(!m_slots.empty(), IsGameModeActive(), vigemMissing);
}
