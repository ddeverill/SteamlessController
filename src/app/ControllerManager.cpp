#include "ControllerManager.h"
#include "EventLog.h"
#include "VirtualController.h"
#include "TrackpadMouse.h"
#include "steam/SteamController.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>

// ---------------------------------------------------------------------------
// Slot
// ---------------------------------------------------------------------------

struct ControllerManager::Slot {
    std::wstring                       path;
    SteamController::Transport         transport = SteamController::Transport::Unknown;
    std::unique_ptr<SteamController>   sc;
    std::unique_ptr<VirtualController> vc;
    TrackpadMouse                      trackpad;
    std::thread                        readThread;
    std::atomic<bool>                  readRunning{false};
    bool                               gameModeActive = false;
    SteamController::AccessClaim       accessClaim = SteamController::AccessClaim::Failed;
    int                                lastBatteryPercent = -1;  // -1 = never reported

    // When this slot was last found silent. Probing for a state report costs
    // the full timeout on an empty puck slot, and the acquire path retries in
    // a rapid burst — without this, three empty slots would block the UI
    // thread for that timeout on every one of those attempts.
    std::chrono::steady_clock::time_point lastSilentAt{};

    // Haptic edge-detection state for touch (movement ticks).
    bool    hapticWasRightTouching = false;
    bool    hapticWasLeftTouching  = false;

    // Click haptic state machine — two states per trackpad.
    // WaitingForPress:   will fire the press haptic the first time the click bit
    //                    goes high; ignores any further highs until release fires.
    // WaitingForRelease: will fire the release haptic the first time the click bit
    //                    goes low; ignores any further lows until next press fires.
    // This fires exactly once per downstroke and once per upstroke regardless of
    // how many times the click bit chatters around the threshold.
    enum class ClickState { WaitingForPress, WaitingForRelease };
    ClickState hapticRightClickState = ClickState::WaitingForPress;
    ClickState hapticLeftClickState  = ClickState::WaitingForPress;

    // Previous trackpad positions and accumulated travel for distance-based haptic.
    int16_t hapticPrevRightX       = 0;
    int16_t hapticPrevRightY       = 0;
    float   hapticRightDistAccum   = 0.0f;
    int16_t hapticPrevLeftX        = 0;
    int16_t hapticPrevLeftY        = 0;
    float   hapticLeftDistAccum    = 0.0f;

    // Grace period after first touch — suppresses movement ticks while the press
    // gesture completes so it can't bleed a TICK in just before the CLICK fires.
    // Counts down from TOUCH_GRACE_FRAMES to 0; ticks only fire when it is 0.
    static constexpr int kTouchGraceFrames = 12;  // ~48ms at 250Hz
    int hapticRightTouchGrace = 0;
    int hapticLeftTouchGrace  = 0;

    // Click haptic latch. Press: the firmware click bit must hold true for a
    // couple frames (filters single-frame glitches), then the press haptic
    // fires. Release: the click bit is IGNORED from then on — chatter around
    // the firmware's force threshold can't re-fire anything. The latch only
    // releases (and the release haptic fires) once contact area falls back
    // to genuinely idle — a light resting touch (~300-600), far below any
    // pressing level (~1400+ even when easing off mid-hold, ~4000 at click).
    // An absolute threshold, NOT a fraction of press area: grinding a hard
    // press dips area to 1000-2000, which a relative threshold read as a
    // release, firing press/release pairs in a crunchy stream.
    static constexpr int   kPressConfirmFrames   = 2;     // ~8ms at 250Hz
    static constexpr float kReleaseIdleArea      = 1000.0f;
    static constexpr int   kAreaLowConfirmFrames = 8;     // ~32ms at 250Hz
    int hapticRightClickTrueFrames = 0;
    int hapticLeftClickTrueFrames  = 0;
    int hapticRightAreaLowFrames   = 0;
    int hapticLeftAreaLowFrames    = 0;

    // Movement-accumulator idle reset: if the finger stays still (below the
    // motion deadzone) this many frames, discard accumulated travel so a
    // primed accumulator can't discharge a spurious TICK from press wobble.
    static constexpr int kIdleResetFrames = 30;  // ~120ms at 250Hz
    int hapticRightIdleFrames = 0;
    int hapticLeftIdleFrames  = 0;

    // Post-release grace — after the release haptic fires, the finger is
    // still peeling off / settling; the centroid jumps hundreds of units per
    // frame during that window, which reads as motion but isn't swiping.
    static constexpr int kPostReleaseGraceFrames = 45;  // ~180ms at 250Hz
    int hapticRightReleaseGrace = 0;
    int hapticLeftReleaseGrace  = 0;

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
// Crash-path lizard restore
//
// If the process dies with the controller in game mode, the user is left
// without input until the firmware's own lizard revert timeout kicks in
// (several seconds). The unhandled-exception filter shortens that window by
// best-effort sending the lizard restore reports on the way down. It must not
// take locks or join threads — another thread may be wedged holding them.
// ---------------------------------------------------------------------------

static ControllerManager* g_crashRestoreInstance = nullptr;

static LONG WINAPI CrashRestoreFilter(EXCEPTION_POINTERS*) {
    if (g_crashRestoreInstance)
        g_crashRestoreInstance->EmergencyRestoreAll();
    // Let Windows Error Reporting / debuggers see the crash as usual.
    return EXCEPTION_CONTINUE_SEARCH;
}

void ControllerManager::EmergencyRestoreAll() noexcept {
    for (auto& slot : m_slots) {
        if (!slot || !slot->sc) continue;
        // Stop the read loop cooperatively (no join) so its keepalive can't
        // re-clear the mappings we are about to restore.
        slot->readRunning = false;
        slot->sc->EmergencyLizardRestore();
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

ControllerManager::ControllerManager(StateChangedFn onStateChanged)
    : m_onStateChanged(std::move(onStateChanged))
{
    g_crashRestoreInstance = this;
    SetUnhandledExceptionFilter(CrashRestoreFilter);
    SyncDevices();
}

ControllerManager::~ControllerManager() {
    g_crashRestoreInstance = nullptr;
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

void ControllerManager::ReleaseDevices() {
    if (!m_slots.empty())
        EventLog::Write("RELEASE: closing all device handles (intentional handoff)");
    // Disable game mode first — enables lizard mode and tears down ViGEm while
    // we still hold write access to the device.
    DisableGameMode();
    // Close all device handles. Slot destructors call SteamController::Close()
    // which closes the HID handle, allowing another process to open it.
    m_slots.clear();
    NotifyStateChanged();
}

void ControllerManager::SetSteamPresent(bool present) {
    if (m_steamPresent == present) return;
    m_steamPresent = present;

    if (!present) {
        m_steamConflictAlertShown = false;
        return;
    }

    const bool sharedModeActive = std::any_of(m_slots.begin(), m_slots.end(),
        [](const auto& slot) {
            return slot->gameModeActive
                && slot->accessClaim == SteamController::AccessClaim::Shared;
        });
    if (!sharedModeActive) return;

    EventLog::Write("GAMEMODE: Steam started during shared access; yielding to prevent duplicate input");
    DisableGameMode();
    NotifySteamConflict();
}

void ControllerManager::SetTrackpadMouseEnabled(bool enabled) {
    m_trackpadMouseEnabled = enabled;
    for (auto& slot : m_slots) {
        slot->trackpad.SetTrackpadEnabled(enabled);
        if (slot->vc) slot->vc->SetTrackpadMouseClaim(enabled, m_useLeftTrackpad);
    }
}

void ControllerManager::SetUseLeftTrackpad(bool enabled) {
    m_useLeftTrackpad = enabled;
    for (auto& slot : m_slots) {
        slot->trackpad.SetUseLeftTrackpad(enabled);
        if (slot->vc) slot->vc->SetTrackpadMouseClaim(m_trackpadMouseEnabled, enabled);
    }
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
            EventLog::Write("DISCONNECT: OS removed device (transport=%s, gameMode=%d, lastBattery=%d%%) %ls",
                            SteamController::TransportName((*it)->transport),
                            (*it)->gameModeActive ? 1 : 0,
                            (*it)->lastBatteryPercent, (*it)->path.c_str());
            // Alert only when the controller was in active use; deliberate
            // unplugs while idle (and our own device cycles, which release
            // slots before cycling) stay quiet.
            if ((*it)->gameModeActive && m_alertFn) {
                if ((*it)->transport == SteamController::Transport::Bluetooth)
                    m_alertFn(L"Controller disconnected",
                              L"The Bluetooth connection dropped — the controller "
                              L"powered off, went to sleep, or is out of range. "
                              L"Press the Steam button to reconnect.");
                else
                    m_alertFn(L"Controller disconnected",
                              L"The USB device was removed. Check the cable or dongle — "
                              L"if this happens randomly, Windows USB power saving may be "
                              L"suspending the port.");
            }
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
    slot->path      = path;
    slot->transport = SteamController::TransportFromPath(path);
    slot->sc        = std::make_unique<SteamController>();
    if (!slot->sc->Open(path)) return;

    EventLog::Write("CONNECT: transport=%s %ls",
                    SteamController::TransportName(slot->transport), path.c_str());

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
    // The puck publishes a controller interface per slot and current firmware
    // rejects the lizard-mode command on an empty one, so only act on a slot
    // that is actually emitting state. Recently-silent slots are skipped
    // outright — see Slot::lastSilentAt.
    static constexpr auto kSilentSlotRetryGap = std::chrono::milliseconds(1500);
    const auto now = std::chrono::steady_clock::now();
    if (now - slot.lastSilentAt < kSilentSlotRetryGap) return;

    if (!slot.sc->WaitForStateReport(250)) {
        slot.lastSilentAt = std::chrono::steady_clock::now();
        EventLog::Write("GAMEMODE: no state reports from slot (transport=%s), skipping %ls",
                        SteamController::TransportName(slot.transport), slot.path.c_str());
        return;
    }

    slot.accessClaim = SteamController::AccessClaim::Failed;
    const auto claim = slot.sc->ClaimGameModeAccess();
    if (claim == SteamController::AccessClaim::Failed) {
        EventLog::Write("GAMEMODE: device reopen failed %ls", slot.path.c_str());
        return;
    }
    if (claim == SteamController::AccessClaim::Shared) {
        // ERROR_SHARING_VIOLATION does not identify the existing owner. Live
        // testing shows that sharing with an idle Steam client is still unsafe:
        // whichever mapper starts second can produce duplicate input or starve
        // the other reader. The Windows-only holder is benign, so shared access
        // remains the compatibility path while Steam is absent.
        if (m_steamPresent) {
            EventLog::Write("GAMEMODE: shared access blocked while Steam is running %ls",
                            slot.path.c_str());
            NotifySteamConflict();
            return;
        }
        EventLog::Write("GAMEMODE: exclusive claim unavailable; using shared access %ls",
                        slot.path.c_str());
    }
    slot.accessClaim = claim;
    if (!slot.sc->DisableLizardMode()) {
        EventLog::Write("GAMEMODE: DisableLizardMode failed %ls", slot.path.c_str());
        slot.accessClaim = SteamController::AccessClaim::Failed;
        slot.sc->ReleaseToShared();
        return;
    }

    slot.vc = std::make_unique<VirtualController>(
        m_controllerPlatform,
        [sc = slot.sc.get()](uint8_t largeMotor, uint8_t smallMotor) {
            if (sc->IsOpen()) sc->SetRumble(largeMotor, smallMotor);
        });
    if (!slot.vc->IsValid()) {
        EventLog::Write("GAMEMODE: ViGEm virtual controller failed (driverMissing=%d)",
                        slot.vc->IsDriverMissing() ? 1 : 0);
        if (slot.vc->IsDriverMissing()) vigemMissingOut = true;
        slot.vc.reset();
        slot.sc->EnableLizardMode();
        slot.accessClaim = SteamController::AccessClaim::Failed;
        return;
    }

    if (m_controllerPlatform == ControllerPlatform::PlayStation)
        slot.sc->SetImuEnabled(true);
    slot.vc->SetTrackpadMouseClaim(m_trackpadMouseEnabled, m_useLeftTrackpad);

    EventLog::Write("GAMEMODE: enabled %ls", slot.path.c_str());
    slot.gameModeActive = true;
    slot.trackpad.Reset();
    slot.trackpad.SetTrackpadEnabled(m_trackpadMouseEnabled);
    slot.trackpad.SetUseLeftTrackpad(m_useLeftTrackpad);
    slot.trackpad.SetBackButtonsEnabled(m_backButtonsEnabled);
    StartReadLoop(slot);
}

void ControllerManager::DisableGameModeSlot(Slot& slot) {
    if (!slot.gameModeActive) return;
    EventLog::Write("GAMEMODE: disabled %ls", slot.path.c_str());
    StopReadLoop(slot);
    if (slot.sc->IsOpen())
        slot.sc->SetRumble(0, 0);
    slot.trackpad.Reset();
    slot.vc.reset();
    slot.sc->EnableLizardMode();
    // Reopen shared so Steam can obtain write access — game mode is no longer active.
    slot.sc->ReleaseToShared();
    slot.accessClaim = SteamController::AccessClaim::Failed;
    slot.gameModeActive = false;
}

void ControllerManager::NotifySteamConflict() {
    if (m_steamConflictAlertShown) return;
    m_steamConflictAlertShown = true;
    if (m_alertFn) {
        m_alertFn(L"Steam is using the controller",
                  L"Windows only granted shared controller access while Steam is running. "
                  L"Steamless Mode was kept off to prevent duplicate or missing input. "
                  L"Exit Steam completely, then enable Steamless Mode again.");
    }
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
    auto    lastKeepalive    = std::chrono::steady_clock::now();
    auto    lastReport       = std::chrono::steady_clock::now();
    bool    stalled          = false;
    bool    keepaliveFailing = false;

    // No reports for this long while the device is still enumerated means the
    // controller died silently: battery empty, auto-sleep, or wireless drop.
    static constexpr auto kStallThreshold = std::chrono::seconds(4);

    while (slot->readRunning) {
        // Keepalive — the firmware silently reverts to lizard mode (and its
        // autonomous click haptics, which double up with ours as a crunchy
        // burst) after a period without host feature reports. Re-assert the
        // lizard-off state every couple of seconds. Must run before the
        // read-timeout continue below, or an idle controller would starve it.
        const auto nowKa = std::chrono::steady_clock::now();
        if (nowKa - lastKeepalive >= std::chrono::seconds(2)) {
            lastKeepalive = nowKa;
            const bool ok = slot->sc->SendKeepalive();
            if (!ok && !keepaliveFailing) {
                keepaliveFailing = true;
                EventLog::Write("KEEPALIVE: send failed (device write error)");
            } else if (ok && keepaliveFailing) {
                keepaliveFailing = false;
                EventLog::Write("KEEPALIVE: recovered");
            }
        }

        size_t n = slot->sc->ReadReport(buf, sizeof(buf), /*timeoutMs=*/32);
        if (n == 0) {
            if (!stalled
                    && std::chrono::steady_clock::now() - lastReport > kStallThreshold) {
                stalled = true;
                EventLog::Write("STALL: no reports for 4s (lastBattery=%d%%) %ls",
                                slot->lastBatteryPercent, slot->path.c_str());
                if (m_alertFn) {
                    wchar_t text[256];
                    if (slot->lastBatteryPercent >= 0)
                        swprintf_s(text,
                                   L"No input received for several seconds — the battery may "
                                   L"be dead, the controller may have gone to sleep, or the "
                                   L"wireless signal dropped. Last battery report: %d%%.",
                                   slot->lastBatteryPercent);
                    else
                        swprintf_s(text,
                                   L"No input received for several seconds — the battery may "
                                   L"be dead, the controller may have gone to sleep, or the "
                                   L"wireless signal dropped.");
                    m_alertFn(L"Controller not responding", text);
                }
            }
            continue;
        }
        lastReport = std::chrono::steady_clock::now();
        if (stalled) {
            stalled = false;
            EventLog::Write("STALL: reports resumed");
        }

        // Battery status — update DS4 battery level; no further processing needed.
        if (buf[0] == SteamController::REPORT_BATTERY_STATUS) {
            if (n >= 3) {
                // USB/dongle payload order is [percent, chargeState]; over
                // Bluetooth the firmware sends the same two bytes swapped —
                // observed BT reports held a constant 1 (CHARGE_STATE_DISCHARGING)
                // in buf[1] while buf[2] tracked the real charge level.
                uint8_t percent     = buf[1];
                uint8_t chargeState = buf[2];
                if (slot->transport == SteamController::Transport::Bluetooth)
                    std::swap(percent, chargeState);
                if (slot->vc)
                    slot->vc->SetBatteryState(percent, chargeState);
                if (slot->lastBatteryPercent != static_cast<int>(percent)) {
                    EventLog::Write("BATTERY: %u%% (chargeState=%u)", percent, chargeState);
                    slot->lastBatteryPercent = static_cast<int>(percent);
                }
            }
            continue;
        }

        if (!SteamController::IsStateReportId(buf[0])) continue;

        if (slot->vc) slot->vc->Update(buf, n, m_backConfig, m_backButtonsEnabled);
        slot->trackpad.Update(buf, n);

        // Trackpad haptics.
        {
            // Distance of deliberate travel between movement ticks. A full
            // top-to-bottom swipe (~65k units) should produce roughly 10 ticks.
            static constexpr float TRACKPAD_HAPTIC_TICK_DISTANCE = 6500.0f;
            // Per-frame motion deadzone: deltas smaller than this are sensor
            // jitter or fingertip wobble from pressing, not deliberate motion.
            // Discarding them keeps the accumulator from priming itself while
            // the finger rests or presses, which caused spurious ticks to fire
            // alongside the click haptic on the downstroke.
            static constexpr float TRACKPAD_HAPTIC_MOTION_DEADZONE = 45.0f;

            const uint8_t b2 = n > 4 ? buf[4] : 0;
            const uint8_t b3 = n > 5 ? buf[5] : 0;
            const bool rt = (b2 & SteamController::BTN_TP_RT)       != 0;
            const bool lt = (b3 & SteamController::BTN_TP_LT)       != 0;
            const bool rc = (b2 & SteamController::BTN_TP_RT_CLICK) != 0;
            const bool lc = (b3 & SteamController::BTN_TP_LT_CLICK) != 0;

            int16_t  rx = 0, ry = 0, lx = 0, ly = 0;
            uint16_t rArea = 0, lArea = 0;
            if (n >= 28) {
                memcpy(&rx, buf + 24, 2);
                memcpy(&ry, buf + 26, 2);
                memcpy(&lx, buf + 18, 2);
                memcpy(&ly, buf + 20, 2);
            }
            if (n >= 30) {
                memcpy(&lArea, buf + 22, 2);
                memcpy(&rArea, buf + 28, 2);
            }

            // Click haptic latch — press fires off the (briefly confirmed)
            // firmware click bit; from then on the click bit is ignored so
            // threshold chatter can't re-fire. The release haptic fires once
            // contact area returns to idle.
            slot->hapticRightClickTrueFrames = rc ? slot->hapticRightClickTrueFrames + 1 : 0;
            slot->hapticLeftClickTrueFrames  = lc ? slot->hapticLeftClickTrueFrames  + 1 : 0;

            if (slot->hapticRightClickState == Slot::ClickState::WaitingForPress
                    && slot->hapticRightClickTrueFrames >= Slot::kPressConfirmFrames) {
                slot->sc->PulseTrackpadHaptic(false, true);
                slot->hapticRightClickState    = Slot::ClickState::WaitingForRelease;
                slot->hapticRightAreaLowFrames = 0;
                slot->hapticPrevRightX        = rx;
                slot->hapticPrevRightY        = ry;
                slot->hapticRightDistAccum    = 0.0f;
            } else if (slot->hapticRightClickState == Slot::ClickState::WaitingForRelease) {
                if (!rc && static_cast<float>(rArea) <= Slot::kReleaseIdleArea) {
                    if (++slot->hapticRightAreaLowFrames >= Slot::kAreaLowConfirmFrames) {
                        slot->sc->PulseTrackpadHaptic(false, true);
                        slot->hapticRightClickState  = Slot::ClickState::WaitingForPress;
                        slot->hapticRightReleaseGrace = Slot::kPostReleaseGraceFrames;
                        slot->hapticPrevRightX      = rx;
                        slot->hapticPrevRightY      = ry;
                        slot->hapticRightDistAccum  = 0.0f;
                    }
                } else {
                    slot->hapticRightAreaLowFrames = 0;
                }
            }

            if (slot->hapticLeftClickState == Slot::ClickState::WaitingForPress
                    && slot->hapticLeftClickTrueFrames >= Slot::kPressConfirmFrames) {
                slot->sc->PulseTrackpadHaptic(true, true);
                slot->hapticLeftClickState    = Slot::ClickState::WaitingForRelease;
                slot->hapticLeftAreaLowFrames = 0;
                slot->hapticPrevLeftX         = lx;
                slot->hapticPrevLeftY         = ly;
                slot->hapticLeftDistAccum     = 0.0f;
            } else if (slot->hapticLeftClickState == Slot::ClickState::WaitingForRelease) {
                if (!lc && static_cast<float>(lArea) <= Slot::kReleaseIdleArea) {
                    if (++slot->hapticLeftAreaLowFrames >= Slot::kAreaLowConfirmFrames) {
                        slot->sc->PulseTrackpadHaptic(true, true);
                        slot->hapticLeftClickState   = Slot::ClickState::WaitingForPress;
                        slot->hapticLeftReleaseGrace = Slot::kPostReleaseGraceFrames;
                        slot->hapticPrevLeftX      = lx;
                        slot->hapticPrevLeftY      = ly;
                        slot->hapticLeftDistAccum  = 0.0f;
                    }
                } else {
                    slot->hapticLeftAreaLowFrames = 0;
                }
            }

            // Movement haptic — tick once per TRACKPAD_HAPTIC_TICK_DISTANCE of travel.
            if (n >= 28) {
                // On first-touch frame, seed positions, reset accumulator, and
                // start grace period so press-gesture motion can't bleed a TICK
                // just before the CLICK fires (~48ms at 250Hz).
                if (rt && !slot->hapticWasRightTouching) {
                    slot->hapticPrevRightX     = rx;
                    slot->hapticPrevRightY     = ry;
                    slot->hapticRightDistAccum = 0.0f;
                    slot->hapticRightTouchGrace = Slot::kTouchGraceFrames;
                }
                if (lt && !slot->hapticWasLeftTouching) {
                    slot->hapticPrevLeftX     = lx;
                    slot->hapticPrevLeftY     = ly;
                    slot->hapticLeftDistAccum = 0.0f;
                    slot->hapticLeftTouchGrace = Slot::kTouchGraceFrames;
                }

                if (slot->hapticRightTouchGrace   > 0) --slot->hapticRightTouchGrace;
                if (slot->hapticLeftTouchGrace    > 0) --slot->hapticLeftTouchGrace;
                if (slot->hapticRightReleaseGrace > 0) --slot->hapticRightReleaseGrace;
                if (slot->hapticLeftReleaseGrace  > 0) --slot->hapticLeftReleaseGrace;

                // No ticks while the finger is pressing down (contact area
                // balloons far past light-touch levels) and no single-frame
                // centroid teleports (partial-contact artifacts) — neither is
                // deliberate swiping, however much distance they report.
                // NOTE: no minimum-area gate — a light gliding finger reads
                // near-zero contact area on this pad, indistinguishable from
                // post-lift ghost drift. Ghosts near clicks are covered by the
                // post-release grace instead.
                static constexpr float TRACKPAD_TICK_MAX_AREA = 900.0f;
                static constexpr float TRACKPAD_TICK_MAX_STEP = 4000.0f;

                // Gate on the latch state, not the raw click bit — while the
                // click is held (even if the bit chatters low), no ticks.
                if (rt && slot->hapticWasRightTouching && !rc
                        && slot->hapticRightClickState == Slot::ClickState::WaitingForPress
                        && slot->hapticRightTouchGrace == 0) {
                    const float dx   = static_cast<float>(rx - slot->hapticPrevRightX);
                    const float dy   = static_cast<float>(ry - slot->hapticPrevRightY);
                    const float dist = std::hypot(dx, dy);
                    if (slot->hapticRightReleaseGrace > 0
                            || static_cast<float>(rArea) > TRACKPAD_TICK_MAX_AREA
                            || dist > TRACKPAD_TICK_MAX_STEP) {
                        // Settling after a release, pressing down, or a centroid
                        // teleport — pause, don't punish: skip the frame without
                        // accumulating it, but keep travel already earned so a
                        // brief excursion mid-swipe doesn't zero the progress.
                        // No tick can fire from a gated frame, and the press
                        // haptic still resets the accumulator when a click lands.
                        slot->hapticPrevRightX = rx;
                        slot->hapticPrevRightY = ry;
                    } else if (dist >= TRACKPAD_HAPTIC_MOTION_DEADZONE) {
                        // Below the deadzone: don't accumulate, and don't advance
                        // the reference point — slow creep past the deadzone still
                        // counts. If the finger stays still long enough, discard
                        // accumulated travel (idle reset below).
                        slot->hapticRightIdleFrames = 0;
                        slot->hapticRightDistAccum += dist;
                        if (slot->hapticRightDistAccum >= TRACKPAD_HAPTIC_TICK_DISTANCE) {
                            if (slot->sc->TickTrackpadMovement(false)) {
                                slot->hapticRightDistAccum = 0.0f;
                            } else {
                                // Rate-limited: hold at threshold so the tick
                                // fires as soon as the limiter reopens.
                                slot->hapticRightDistAccum = TRACKPAD_HAPTIC_TICK_DISTANCE;
                            }
                        }
                        slot->hapticPrevRightX = rx;
                        slot->hapticPrevRightY = ry;
                    } else if (++slot->hapticRightIdleFrames >= Slot::kIdleResetFrames) {
                        slot->hapticRightDistAccum = 0.0f;
                        slot->hapticPrevRightX     = rx;
                        slot->hapticPrevRightY     = ry;
                    }
                }
                if (lt && slot->hapticWasLeftTouching && !lc
                        && slot->hapticLeftClickState == Slot::ClickState::WaitingForPress
                        && slot->hapticLeftTouchGrace == 0) {
                    const float dx   = static_cast<float>(lx - slot->hapticPrevLeftX);
                    const float dy   = static_cast<float>(ly - slot->hapticPrevLeftY);
                    const float dist = std::hypot(dx, dy);
                    if (slot->hapticLeftReleaseGrace > 0
                            || static_cast<float>(lArea) > TRACKPAD_TICK_MAX_AREA
                            || dist > TRACKPAD_TICK_MAX_STEP) {
                        slot->hapticPrevLeftX = lx;
                        slot->hapticPrevLeftY = ly;
                    } else if (dist >= TRACKPAD_HAPTIC_MOTION_DEADZONE) {
                        slot->hapticLeftIdleFrames = 0;
                        slot->hapticLeftDistAccum += dist;
                        if (slot->hapticLeftDistAccum >= TRACKPAD_HAPTIC_TICK_DISTANCE) {
                            if (slot->sc->TickTrackpadMovement(true))
                                slot->hapticLeftDistAccum = 0.0f;
                            else
                                slot->hapticLeftDistAccum = TRACKPAD_HAPTIC_TICK_DISTANCE;
                        }
                        slot->hapticPrevLeftX = lx;
                        slot->hapticPrevLeftY = ly;
                    } else if (++slot->hapticLeftIdleFrames >= Slot::kIdleResetFrames) {
                        slot->hapticLeftDistAccum = 0.0f;
                        slot->hapticPrevLeftX     = lx;
                        slot->hapticPrevLeftY     = ly;
                    }
                }
            }

            slot->hapticWasRightTouching = rt;
            slot->hapticWasLeftTouching  = lt;
        }

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
