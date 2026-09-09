#include "ControllerManager.h"
#include "EventLog.h"
#include "InputInjection.h"
#include "ViGEmBusInfo.h"
#include "VirtualController.h"
#include "TrackpadInput.h"
#include "KeyInput.h"
#include "TouchKeyboard.h"
#include "steam/SteamController.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <memory>
#include <thread>
#include <utility>

// ---------------------------------------------------------------------------
// Slot
// ---------------------------------------------------------------------------


// Milliseconds since a steady_clock mark. steady_clock is QPC-backed on
// Windows, so this resolves well below the millisecond it reports in.
static double ElapsedMs(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - since).count();
}
struct ControllerManager::Slot {
    std::wstring                       path;
    SteamController::Transport         transport = SteamController::Transport::Unknown;
    std::unique_ptr<SteamController>   sc;
    std::unique_ptr<VirtualController> vc;
    // One per physical pad — they are configured independently.
    TrackpadInput                      leftPad;
    TrackpadInput                      rightPad;
    std::thread                        readThread;
    std::atomic<bool>                  readRunning{false};
    bool                               gameModeActive = false;
    // Game mode came up on a shared handle: another process (in practice Steam)
    // holds a write handle too, so both of us are driving this controller. Only
    // meaningful while gameModeActive — it is what the tray reports.
    bool                               sharedHandle = false;
    // Result of the claim sweep that runs before any slot is probed, so the
    // per-slot path can use it rather than claiming on its own. Unset when a
    // slot is enabled outside a sweep — a controller switched on while game
    // mode is already running — in which case it claims for itself.
    bool                               hasPendingClaim = false;
    SteamController::AccessClaim       pendingClaim =
                                           SteamController::AccessClaim::Failed;
    int                                lastBatteryPercent = -1;  // -1 = never reported

    // When this slot was last found silent. Probing for a state report costs
    // the full timeout on an empty puck slot, and the acquire path retries in
    // a rapid burst — without this, three empty slots would block the UI
    // thread for that timeout on every one of those attempts.
    std::chrono::steady_clock::time_point lastSilentAt{};

    // What each bindable button is currently holding down, so leaving game
    // mode mid-press releases it instead of stranding the input down. Records
    // what the press actually sent rather than reading the binding again on
    // release, so rebinding mid-hold still releases cleanly.
    //
    // Indices 0-3 are the paddles (L4, L5, R4, R5); everything after them
    // belongs to the trackpads, which are ordinary bindings dispatched down
    // this same path. 4-5 are the pad clicks, 6-7 the pad touches, and 8-15
    // the four directions of each pad in turn.
    static constexpr size_t kBindableCount = 16;
    BackButtonBinding paddleHeld[kBindableCount];

    // What each pad's directions were last frame. Unlike every other edge
    // dispatched here these are not bits in the report — they are resolved
    // from a position, so the previous report cannot be re-read for them and
    // the answer has to be carried. Written every frame, including the first,
    // so a direction cannot be stranded down across a mode change.
    uint8_t prevLeftDirs  = DirNone;
    uint8_t prevRightDirs = DirNone;

    // Tap detection, and the previous answer for the same reason as the
    // directions above.
    //
    // The report's touch bit means "pad active", which a press sets on its way
    // down — the finger lands, force builds, and only then does the click bit
    // follow. A binding dispatched off the raw bit therefore fired on every
    // press, before the press itself, in every mode. Touch is not an
    // independent signal: a click always implies one.
    //
    // Waiting a fixed time to see whether a click follows does not rescue it.
    // Measured with TrackpadZoneProbe over 148 presses, the finger is down for
    // a median of 59 frames before the click and sometimes thousands — a
    // thumb is planted and then pressed. Any window short enough for a tap to
    // feel responsive fires before nearly every press.
    //
    // So a tap is decided on the lift instead, where it is not a guess at all:
    // a contact that ended without a click, was brief, and did not travel.
    // That is what a tap is, and all three are known by then. It reports as a
    // short pulse rather than a hold, because the event being reported is over
    // by the time it is known to have happened.
    static constexpr int kTapMaxFrames  = 50;    // ~200ms — a tap is quick
    static constexpr int kTapMaxTravel  = 3000;  // and lands in one place
    static constexpr int kTapPulseFrames = 12;   // ~48ms, long enough to register
    struct TapState {
        int     frames   = 0;      // frames the finger has been down
        bool    sawClick = false;
        int16_t startX   = 0;
        int16_t startY   = 0;
        int     travel   = 0;      // furthest it has strayed from where it landed
        int     pulse    = 0;      // frames left in the pulse a tap fires

        // Returns whether the tap binding should be held this frame.
        bool Update(bool touching, bool clicked, int16_t x, int16_t y) {
            if (!touching) {
                if (frames > 0 && !sawClick
                        && frames <= kTapMaxFrames && travel <= kTapMaxTravel)
                    pulse = kTapPulseFrames;
                frames   = 0;
                sawClick = false;
                travel   = 0;
            } else {
                if (frames == 0) { startX = x; startY = y; travel = 0; }
                if (clicked) sawClick = true;
                ++frames;
                const int dx = x - startX, dy = y - startY;
                const int d  = static_cast<int>(std::sqrt(
                    static_cast<double>(dx) * dx + static_cast<double>(dy) * dy));
                if (d > travel) travel = d;
            }
            if (pulse > 0) { --pulse; return true; }
            return false;
        }
    };
    TapState leftTap;
    TapState rightTap;
    bool prevLeftTap  = false;
    bool prevRightTap = false;

    // Is the pad being pressed? Worked out from contact area rather than taken
    // from the firmware's click bit — see kPadPressArea. The click bit is still
    // consulted, because when it does fire it is right; it just misses more
    // than half of what a thumb does.
    struct PressState {
        bool pressed = false;
        int  above   = 0;
        int  below   = 0;

        bool Update(uint16_t area, bool clickBit) {
            const bool raw = area >= kPadPressArea || clickBit;
            if (raw) { ++above; below = 0; } else { ++below; above = 0; }
            if (!pressed && above >= kPadPressFrames)      pressed = true;
            else if (pressed && below >= kPadPressFrames)  pressed = false;
            return pressed;
        }
    };
    PressState leftPress;
    PressState rightPress;
    bool prevLeftPressed  = false;
    bool prevRightPressed = false;

    // Auto-repeat for held key bindings. When the next repeat is due, and the
    // gap to use after that — both captured at press time from the user's
    // keyboard settings, so the rate cannot shift mid-hold.
    std::chrono::steady_clock::time_point paddleRepeatAt[kBindableCount]{};
    std::chrono::milliseconds             paddleRepeatGap[kBindableCount]{};

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
    // fires. Release: the click bit must hold false for a few more, then the
    // release haptic fires and the latch re-arms. Both edges are the same
    // idea — believe the click bit once it has held an answer long enough to
    // not be threshold chatter.
    //
    // Release used to wait on contact area falling to an absolute idle level
    // instead, on the reasoning that the click bit chatters around the
    // firmware's force threshold and area does not. It does not chatter, but
    // it also never falls that far unless the thumb leaves the pad: measured
    // with TrackpadZoneProbe, a thumb resting between two rapid taps sits
    // around 2500, and the lowest a real tap gap reached was 1236 against a
    // threshold of 1000. So every press after the first in a burst found the
    // latch still waiting and fired no haptic at all — which is fine for a
    // pad being clicked like a mouse button, and useless for one being
    // tapped like a d-pad.
    //
    // Duration is what separates chatter from a release, and area is actively
    // misleading for it: grinding a hard press dips area to 1000-2000, which
    // is LOWER than the planted thumb between taps that has to count as a
    // release. The same probe measured a real gap holding the click bit low
    // for 18 frames, so four is a wide margin below anything deliberate.
    static constexpr int kPressConfirmFrames   = 2;  // ~8ms at 250Hz
    static constexpr int kReleaseConfirmFrames = 4;  // ~16ms at 250Hz
    int hapticRightClickTrueFrames  = 0;
    int hapticLeftClickTrueFrames   = 0;
    int hapticRightClickLowFrames   = 0;
    int hapticLeftClickLowFrames    = 0;

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
    // Controller input only ever yields a built-in action; the caller wraps the
    // result. Keyboard and mouse capture arrive through the UI, not this loop.
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
    StopPounce();
    for (auto& slot : m_slots) {
        // Stop the read loop and virtual controller first (same as DisableGameModeSlot
        // but without the gameModeActive guard — we always want to restore lizard mode).
        if (slot->gameModeActive) {
            StopReadLoop(*slot);
            slot->leftPad.Reset();
            slot->rightPad.Reset();
            ReleaseHeldPaddleInputs(*slot);
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

bool ControllerManager::IsGameModeShared() const {
    return std::any_of(m_slots.begin(), m_slots.end(),
        [](const auto& s) { return s->gameModeActive && s->sharedHandle; });
}

void ControllerManager::OnDeviceChange() {
    SyncDevices();
}

ControllerManager::GameModeOutcome ControllerManager::EnableGameMode(uint32_t stateWaitMs,
                                                                     bool allowShared) {
    // Once the absence of a bus driver is established, re-establish it the
    // cheap way. Finding out by attempting an enable costs the user's
    // controller: each attempt claims the device exclusively and toggles
    // lizard mode off and back on, and the retry that waits for a driver to
    // be installed would otherwise do that every 30 seconds for as long as
    // the machine goes without one. Enumerating touches no device at all.
    if (m_lastPadDriverMissing && ViGEmBusInfo::Enumerate().empty()) {
        NotifyStateChanged(true);
        return GameModeOutcome::VirtualPadUnavailable;
    }

    // Claim first, ask which slot is live second. A live slot answers a state
    // probe within a report or two, but an empty one costs the whole probe
    // timeout — so walking the list probe-first leaves the live slot unclaimed
    // until every empty slot ahead of it has timed out. With three empty slots
    // that is the difference between claiming ~0ms and ~75ms after the device
    // arrives, and it is the window Steam uses to reopen the device after a
    // cycle: measured, Steam reclaims at ~180ms here and under 73ms on the
    // machine in #79, which is why the same code wins the race on one and
    // never on the other.
    //
    // Claiming an empty slot costs nothing and is given straight back below
    // when it turns out to be silent.
    const auto claimStart = std::chrono::steady_clock::now();
    for (auto& slot : m_slots) {
        if (slot->gameModeActive) continue;
        slot->pendingClaim    = slot->sc->ClaimGameModeAccess();
        slot->hasPendingClaim = true;
    }
    m_timing.claimMs += ElapsedMs(claimStart);

    bool anyPadUnavailable = false;
    bool anyBlocked        = false;
    bool anyEnabled        = false;

    auto pass = [&](uint32_t waitMs, bool recordSilent) {
        for (auto& slot : m_slots) {
            switch (EnableGameModeSlot(*slot, anyPadUnavailable, allowShared, waitMs,
                                       recordSilent)) {
            case GameModeOutcome::Enabled:               anyEnabled = true; break;
            case GameModeOutcome::Blocked:               anyBlocked = true; break;
            case GameModeOutcome::VirtualPadUnavailable: break;  // tracked by the out-param
            case GameModeOutcome::NoActiveController:    break;
            }
        }
    };

    // Two passes, because reaching the claim quickly is what wins the controller
    // back after a device cycle. A receiver publishes an interface per slot and
    // only one has a controller in it; a live slot streams continuously and
    // answers a state probe within a few reports, while an empty one costs the
    // whole timeout. Probing every slot at the full timeout therefore spends up
    // to 750ms on dead slots before it even tries to claim the live one —
    // measured — and Steam reclaims the device about 180ms after it re-arrives.
    // The quick pass gets the claim inside that window; the full pass is the
    // fallback for a controller that is slow to resume streaming.
    //
    // The quick pass deliberately does not record slots as silent: a slot that
    // simply had not started streaming yet must not be skipped by the full pass
    // that follows, nor logged as absent when it is merely early.
    static constexpr uint32_t kQuickProbeMs = 25;
    const uint32_t quickMs = (std::min)(kQuickProbeMs, stateWaitMs);
    pass(quickMs, /*recordSilent=*/quickMs == stateWaitMs);
    if (!anyEnabled && quickMs < stateWaitMs)
        pass(stateWaitMs, /*recordSilent=*/true);

    NotifyStateChanged(anyPadUnavailable);
    if (anyEnabled) return GameModeOutcome::Enabled;
    // Only report Blocked when something actually stood in the way. Slots that
    // are merely silent mean no controller is switched on, which no amount of
    // device cycling will change.
    //
    // Blocked outranks a missing pad: contention is the one of the two a
    // device cycle can still resolve, so if any slot is contested the caller
    // should hear about that rather than settle into the slow ViGEm retry.
    if (anyBlocked)        return GameModeOutcome::Blocked;
    if (anyPadUnavailable) return GameModeOutcome::VirtualPadUnavailable;
    return GameModeOutcome::NoActiveController;
}

void ControllerManager::DisableGameMode() {
    for (auto& slot : m_slots)
        DisableGameModeSlot(*slot);
    NotifyStateChanged();
}

void ControllerManager::ReleaseDevices() {
    // Not StopPounce(): a release is exactly what precedes a cycle, and the
    // pounce is armed to survive it. TrayApp stops it when the acquire ends.
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


// One pad, in the terms a bug report is written in.
//
// Scroll carries more than the others because a scroll-feels-wrong report has
// two independent multipliers behind it, and neither alone explains the feel:
// the user's own speed setting, and the machine's lines-per-notch, which
// Windows applies to every wheel event after we send it. The net figure is
// what the pad actually does relative to the calibrated feel, and it is the
// number worth comparing between two machines.
static std::string DescribePad(const TrackpadSettings& pad) {
    switch (pad.mode) {
    case TrackpadMode::MousePointer:   return "pointer";
    case TrackpadMode::DS4Touchpad:    return "ds4 touchpad";
    case TrackpadMode::SingleButton:   return "single button";
    case TrackpadMode::DirectionalPad:
        return std::string("dpad (")
             + (pad.diagonals == DiagonalMode::FourWay ? "4-way" : "8-way") + ")";
    case TrackpadMode::ScrollWheel: {
        const float correction =
            InputInjection::WheelCorrection(InputInjection::WheelLinesPerNotch());
        const char* dir = pad.scrollDir == ScrollDirection::Reversed ? "reversed"
                                                                    : "natural";
        char buf[160];
        // The correction is 1.0 on a machine left at the Windows default, where
        // repeating it and the net figure would say the same thing three times.
        if (std::fabs(correction - 1.0f) < 0.005f)
            snprintf(buf, sizeof(buf), "scroll (%s, speed %u%%)", dir, pad.scrollSpeed);
        else
            snprintf(buf, sizeof(buf),
                     "scroll (%s, speed %u%%, wheel correction x%.2f, net %.0f%% of "
                     "calibrated)",
                     dir, pad.scrollSpeed, static_cast<double>(correction),
                     static_cast<double>(pad.scrollSpeed) * correction);
        return buf;
    }
    default: return "none";
    }
}

void ControllerManager::LogPadSettings() {
    std::string line = "PADS: left=" + DescribePad(m_profile.leftPad)
                     + " right="     + DescribePad(m_profile.rightPad);

    // The failure this exists to make visible: settings changed with game mode
    // off do nothing at all, and until now nothing said so. Two #95 sessions
    // were spent that way — one where the pad was never claimed, one where it
    // was configured after the claim.
    if (!IsGameModeActive())
        line += " — game mode is off, so these are not driving anything yet";

    if (line == m_lastPadDescription) return;
    m_lastPadDescription = line;
    // %s rather than the line as a format: it contains per-cent signs.
    EventLog::Write("%s", line.c_str());
}

void ControllerManager::ApplyPadSettings(Slot& slot) {
    slot.leftPad.SetPad(true);
    slot.rightPad.SetPad(false);
    slot.leftPad.SetMode(m_profile.leftPad.mode);
    slot.rightPad.SetMode(m_profile.rightPad.mode);
    slot.leftPad.SetScrollDirection(m_profile.leftPad.scrollDir);
    slot.rightPad.SetScrollDirection(m_profile.rightPad.scrollDir);
    slot.leftPad.SetScrollSpeed(m_profile.leftPad.scrollSpeed);
    slot.rightPad.SetScrollSpeed(m_profile.rightPad.scrollSpeed);
    slot.leftPad.SetDiagonals(m_profile.leftPad.diagonals);
    slot.rightPad.SetDiagonals(m_profile.rightPad.diagonals);
    // The virtual controller reads pad modes straight off the profile it is
    // handed every frame, so there is nothing to push to it here.
}

void ControllerManager::SetProfile(const ControllerProfile& profile) {
    const bool platformChanged = profile.platform != m_profile.platform;
    m_profile = profile;

    // The virtual pad is created as an X360 target or a DS4 one and cannot
    // become the other, so a platform change is the one setting here that
    // needs the controller rebuilt rather than merely re-read. Cheap to get
    // wrong in the other direction, too: rebuilding on every apply would
    // disconnect and reconnect the pad under the running game each time
    // somebody edited a paddle binding.
    if (platformChanged && IsGameModeActive()) {
        const bool wasShared = IsGameModeShared();
        DisableGameMode();
        // Rebuilding a game mode that was already running: the caller changed a
        // setting, not the acquire policy. Let it come back on a shared handle
        // if that is what it had (or all that is left) rather than dropping the
        // user's controller entirely over a platform toggle.
        EnableGameMode(250, /*allowShared=*/wasShared);  // re-applies pad settings per slot
        return;
    }

    // Live update: the ReadLoop reads m_profile on every frame, so paddle and
    // pad-click changes take effect on the very next report. Movement modes
    // live inside the per-slot TrackpadInput objects, so those are pushed.
    for (auto& slot : m_slots)
        ApplyPadSettings(*slot);

    // After the push, so what is logged is what the pads are now set to.
    LogPadSettings();
}

// Sends the key or mouse event a binding stands for. Gamepad actions reach the
// virtual pad instead and are ignored here. Returns whether anything was sent.
static bool SendPaddleInput(const BackButtonBinding& binding, bool down) {
    auto sendMouse = [](DWORD flags) {
        INPUT inp{};
        inp.type       = INPUT_MOUSE;
        inp.mi.dwFlags = flags;
        InputInjection::Send(inp, "paddle-mouse");
    };

    switch (binding.kind) {
    case BackButtonBinding::Kind::Key:
        // Modifiers wrap the key: down before it so the application sees them
        // already held when the key arrives, up after it so the shortcut is
        // never briefly the unmodified key on the way out.
        if (down) {
            SendModifiers(binding.mods, true);
            SendKeyInput(binding.code, true);
        } else {
            SendKeyInput(binding.code, false);
            SendModifiers(binding.mods, false);
        }
        return true;

    case BackButtonBinding::Kind::MouseButton: {
        INPUT inp{};
        inp.type = INPUT_MOUSE;
        switch (static_cast<BackButtonBinding::MouseButtonCode>(binding.code)) {
        case BackButtonBinding::MouseButtonCode::Middle:
            inp.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        // The thumb buttons share one event pair and are told apart by mouseData.
        case BackButtonBinding::MouseButtonCode::X1:
            inp.mi.dwFlags   = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            inp.mi.mouseData = XBUTTON1;
            break;
        case BackButtonBinding::MouseButtonCode::X2:
            inp.mi.dwFlags   = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            inp.mi.mouseData = XBUTTON2;
            break;
        default:
            return false;
        }
        InputInjection::Send(inp, "paddle-mouse");
        return true;
    }

    case BackButtonBinding::Kind::Action:
        if (binding.IsAction(BackButtonAction::LeftMouseButton)) {
            sendMouse(down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
            return true;
        }
        if (binding.IsAction(BackButtonAction::RightMouseButton)) {
            sendMouse(down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
            return true;
        }
        if (binding.IsAction(BackButtonAction::TouchKeyboard)) {
            // A toggle, not a held input: the press acts and the release has
            // nothing to undo. It still reports true so the caller records it
            // as held, which is what keeps the release path and
            // ReleaseHeldPaddleInputs symmetric with every other binding —
            // both land back here and do nothing. Auto-repeat cannot reach it
            // either, being restricted to Kind::Key, so leaning on the paddle
            // will not flap the keyboard.
            if (down) TouchKeyboard::Toggle();
            return true;
        }
        return false;
    }
    return false;
}

void ControllerManager::ReleaseHeldPaddleInputs(Slot& slot) {
    for (auto& held : slot.paddleHeld) {
        SendPaddleInput(held, false);
        held = BackButtonBinding{};
    }
}

void ControllerManager::StartButtonCapture(std::function<void(const BackButtonBinding&)> callback) {
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


// ---------------------------------------------------------------------------
// Pounce — taking the device back the moment the cycle returns it
// ---------------------------------------------------------------------------

void ControllerManager::BeginPounce() {
    StopPounce();

    {
        std::lock_guard<std::mutex> lk(m_pounce.mutex);
        m_pounce.paths.clear();
        m_pounce.caught.clear();
        for (auto& slot : m_slots)
            m_pounce.paths.push_back(slot->path);
    }
    if (m_pounce.paths.empty()) return;

    m_pounce.running = true;
    m_pounce.thread = std::thread([this] {
        // A path only counts once we have seen it go away. Grabbing before the
        // cycle takes the devnode down would hold the very handle that vetoes
        // it — PnP does not care that the handle is ours.
        std::vector<std::wstring> paths;
        {
            std::lock_guard<std::mutex> lk(m_pounce.mutex);
            paths = m_pounce.paths;
        }
        std::vector<bool> seenGone(paths.size(), false);
        std::vector<bool> caught(paths.size(), false);

        const auto start = std::chrono::steady_clock::now();
        static constexpr double kGiveUpMs = 15000.0;

        while (m_pounce.running && ElapsedMs(start) < kGiveUpMs) {
            bool allCaught = true;
            for (size_t i = 0; i < paths.size(); ++i) {
                if (caught[i]) continue;
                allCaught = false;

                // Exclusive from the very first open: opening shared and
                // upgrading afterwards hands the device back for as long as it
                // takes to ask for it again.
                HANDLE h = CreateFileW(paths[i].c_str(), GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_OVERLAPPED, nullptr);
                if (h == INVALID_HANDLE_VALUE) {
                    const DWORD err = GetLastError();
                    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                        seenGone[i] = true;   // the cycle has taken it down
                    continue;
                }
                if (!seenGone[i]) {
                    // Still the pre-cycle device. Let go immediately.
                    CloseHandle(h);
                    continue;
                }
                caught[i] = true;
                const double ms = ElapsedMs(start);
                {
                    std::lock_guard<std::mutex> lk(m_pounce.mutex);
                    m_pounce.caught.emplace_back(paths[i], h);
                }
                EventLog::Write("POUNCE: took the device %.1fms after the cycle started %ls",
                                ms, paths[i].c_str());
            }
            if (allCaught) break;
            SwitchToThread();
        }
    });
}

void ControllerManager::StopPounce() {
    m_pounce.running = false;
    if (m_pounce.thread.joinable())
        m_pounce.thread.join();
    // Anything caught but never adopted has to go back, or we sit on a device
    // nobody is driving.
    std::lock_guard<std::mutex> lk(m_pounce.mutex);
    for (auto& [path, h] : m_pounce.caught)
        if (h) CloseHandle(static_cast<HANDLE>(h));
    m_pounce.caught.clear();
}

bool ControllerManager::AdoptPounced() {
    std::vector<std::pair<std::wstring, void*>> caught;
    {
        std::lock_guard<std::mutex> lk(m_pounce.mutex);
        caught.swap(m_pounce.caught);
    }
    if (caught.empty()) return false;

    bool any = false;
    for (auto& [path, h] : caught) {
        // Already have it? Then the ordinary path got there first; drop ours.
        if (std::any_of(m_slots.begin(), m_slots.end(),
                        [&](const auto& s) { return s->path == path; })) {
            CloseHandle(static_cast<HANDLE>(h));
            continue;
        }
        auto slot = std::make_unique<Slot>();
        slot->path      = path;
        slot->transport = SteamController::TransportFromPath(path);
        slot->sc        = std::make_unique<SteamController>();
        if (!slot->sc->AdoptHandle(h, path)) {
            CloseHandle(static_cast<HANDLE>(h));
            continue;
        }
        // The handle is already exclusive, so the claim sweep must not reopen
        // it — that would give the device back and re-run the race we just won.
        slot->hasPendingClaim = true;
        slot->pendingClaim    = SteamController::AccessClaim::Exclusive;
        EventLog::Write("POUNCE: adopted %ls", path.c_str());
        m_slots.push_back(std::move(slot));
        any = true;
    }
    if (any) NotifyStateChanged();
    return any;
}
void ControllerManager::SyncDevices() {
    const auto enumStart = std::chrono::steady_clock::now();
    auto livePaths = SteamController::EnumerateAll();
    m_timing.enumerateMs += ElapsedMs(enumStart);
    ++m_timing.enumerates;

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
    const auto openStart = std::chrono::steady_clock::now();
    const bool opened = slot->sc->Open(path);
    m_timing.openMs += ElapsedMs(openStart);
    if (!opened) return;

    EventLog::Write("CONNECT: transport=%s %ls",
                    SteamController::TransportName(slot->transport), path.c_str());

    m_slots.push_back(std::move(slot));

    if (IsGameModeActive()) {
        bool dummy = false;
        // Game mode is already running, so the exclusive-versus-shared question
        // was settled when it came up. A slot arriving late should join on
        // whatever it can get rather than re-open that argument — refusing it
        // would silently drop a controller the user just switched on.
        EnableGameModeSlot(*m_slots.back(), dummy, /*allowShared=*/true, 250,
                           /*recordSilent=*/true);
    } else {
        // Restore lizard mode in case a previous session crashed without cleaning up.
        m_slots.back()->sc->EnableLizardMode();
    }
}

// ---------------------------------------------------------------------------
// Game mode per-slot helpers
// ---------------------------------------------------------------------------

ControllerManager::GameModeOutcome
ControllerManager::EnableGameModeSlot(Slot& slot, bool& padUnavailableOut, bool allowShared,
                                      uint32_t stateWaitMs, bool recordSilent) {
    if (slot.gameModeActive) return GameModeOutcome::Enabled;
    // The puck publishes a controller interface per slot and current firmware
    // rejects the lizard-mode command on an empty one, so only act on a slot
    // that is actually emitting state. Recently-silent slots are skipped
    // outright — see Slot::lastSilentAt.
    static constexpr auto kSilentSlotRetryGap = std::chrono::milliseconds(1500);
    const auto now = std::chrono::steady_clock::now();
    if (now - slot.lastSilentAt < kSilentSlotRetryGap) {
        // Known silent, so it is not the slot the sweep was for — give its
        // claim back rather than sitting on a slot we are about to skip.
        if (slot.hasPendingClaim) {
            slot.sc->ReleaseToShared();
            slot.hasPendingClaim = false;
        }
        return GameModeOutcome::NoActiveController;
    }

    const auto probeStart = std::chrono::steady_clock::now();
    const bool live = slot.sc->WaitForStateReport(stateWaitMs);
    m_timing.probeMs += ElapsedMs(probeStart);
    if (live) m_lastLivePath = slot.path;
    if (!live) {
        // Nothing here, so give the claim back. The sweep above takes every
        // slot to get the live one early; holding an empty one exclusively
        // would lock Steam out of a slot we have no use for.
        if (slot.hasPendingClaim) {
            slot.sc->ReleaseToShared();
            slot.hasPendingClaim = false;
        }
        // Only a full-timeout probe is evidence that a slot is genuinely empty.
        // A quick probe that came up short says nothing, so it must not latch
        // the slot out of the pass that follows.
        if (recordSilent) {
            slot.lastSilentAt = std::chrono::steady_clock::now();
            EventLog::Write("GAMEMODE: no state reports from slot (transport=%s), skipping %ls",
                            SteamController::TransportName(slot.transport), slot.path.c_str());
        }
        return GameModeOutcome::NoActiveController;
    }

    // Use the claim the sweep already took. Only a slot enabled outside a sweep
    // (a controller switched on while game mode is running) claims here.
    const auto claim = slot.hasPendingClaim ? slot.pendingClaim
                                            : slot.sc->ClaimGameModeAccess();
    slot.hasPendingClaim = false;
    if (claim == SteamController::AccessClaim::Failed) {
        EventLog::Write("GAMEMODE: device reopen failed %ls", slot.path.c_str());
        return GameModeOutcome::Blocked;
    }
    slot.sharedHandle = (claim == SteamController::AccessClaim::Shared);
    if (claim == SteamController::AccessClaim::Shared) {
        // Someone else holds a write handle — with Steam running that is Steam,
        // which claims the vendor collection when it registers the controller
        // and never lets go. No exclusive claim is obtainable against a live
        // write handle however polite: FILE_SHARE_READ asks the kernel to deny
        // write to everyone else, and it must refuse while one exists.
        //
        // Sharing works — the commands game mode needs are feature reports, and
        // the read loop re-asserts lizard-off every 2s — but it leaves the other
        // process driving the same controller, which doubles up any input it
        // also binds. So refuse first: reporting Blocked is what escalates to a
        // device cycle, and invalidating the other handle is the only way to
        // actually win exclusivity. Only settle once the caller has spent its
        // cycle budget and told us sharing beats having no controller at all.
        if (!allowShared) {
            EventLog::Write("GAMEMODE: exclusive claim blocked — another process holds a "
                            "write handle %ls",
                            slot.path.c_str());
            slot.sharedHandle = false;
            return GameModeOutcome::Blocked;
        }
        EventLog::Write("GAMEMODE: exclusive claim unavailable; using shared access %ls",
                        slot.path.c_str());
    }
    if (!slot.sc->DisableLizardMode()) {
        EventLog::Write("GAMEMODE: DisableLizardMode failed %ls", slot.path.c_str());
        slot.sc->ReleaseToShared();
        return GameModeOutcome::Blocked;
    }

    slot.vc = std::make_unique<VirtualController>(
        m_profile.platform,
        [sc = slot.sc.get()](uint8_t largeMotor, uint8_t smallMotor) {
            if (sc->IsOpen()) sc->SetRumble(largeMotor, smallMotor);
        });
    if (!slot.vc->IsValid()) {
        EventLog::Write("GAMEMODE: ViGEm virtual controller failed (stage=%s, err=0x%08X, driverMissing=%d)",
                        slot.vc->FailStage(), slot.vc->LastError(),
                        slot.vc->IsDriverMissing() ? 1 : 0);
        // The line above says a call failed; this one says what the machine
        // actually looks like. A second bus is the most likely cause of an
        // otherwise inexplicable connect failure and the hardest thing to
        // guess at from the outside, so it goes in the log every time.
        if (!slot.vc->BusReport().empty()) {
            EventLog::Write("GAMEMODE: %ls", slot.vc->BusReport().c_str());
            m_lastBusReport = slot.vc->BusReport();
        }
        m_lastPadDriverMissing = slot.vc->IsDriverMissing();
        padUnavailableOut      = true;
        slot.vc.reset();
        slot.sc->EnableLizardMode();
        // Hand the device back. Without a virtual pad this slot is going
        // nowhere, and an exclusive handle held for nothing locks Steam out
        // and makes the app itself the holder that vetoes its own device
        // cycle — which is the veto the reporter's #65 logs recorded.
        slot.sc->ReleaseToShared();
        return GameModeOutcome::VirtualPadUnavailable;
    }

    // A pad was created, so whatever was missing is not missing now — clear it
    // or the cheap pre-flight above would keep short-circuiting a machine that
    // has since had the driver installed.
    m_lastPadDriverMissing = false;

    if (m_profile.platform == ControllerPlatform::PlayStation)
        slot.sc->SetImuEnabled(true);

    EventLog::Write("GAMEMODE: enabled %ls", slot.path.c_str());
    // Takeover is the moment users report the trackpad arriving dead, and what
    // decides whether injection can land is whatever window happens to be in
    // front right then — so record it here rather than reconstructing it later.
    //
    // Unconditional. It used to fire only when a pad was already claimed for
    // the desktop, which quietly withheld it from the two reports most likely
    // to need it: a pad configured after enabling, and a trackpad that does
    // nothing because no pad is claimed at all. The second is worth a line
    // saying so, not silence.
    InputInjection::LogEnvironment("game mode taken");
    slot.gameModeActive = true;
    slot.leftPad.Reset();
    slot.rightPad.Reset();
    ReleaseHeldPaddleInputs(slot);
    ApplyPadSettings(slot);
    // After gameModeActive, so the line does not claim the pads are inert.
    LogPadSettings();
    StartReadLoop(slot);
    return GameModeOutcome::Enabled;
}

void ControllerManager::DisableGameModeSlot(Slot& slot) {
    if (!slot.gameModeActive) return;
    // Forget what was last logged, so the next enable states the pad setup
    // again rather than deduplicating against a previous session's line. Each
    // enable in the log should be readable without scrolling back past it.
    m_lastPadDescription.clear();
    EventLog::Write("GAMEMODE: disabled %ls", slot.path.c_str());
    StopReadLoop(slot);
    if (slot.sc->IsOpen())
        slot.sc->SetRumble(0, 0);
    slot.leftPad.Reset();
    slot.rightPad.Reset();
    ReleaseHeldPaddleInputs(slot);
    slot.vc.reset();
    slot.sc->EnableLizardMode();
    // Reopen shared so Steam can obtain write access — game mode is no longer active.
    slot.sc->ReleaseToShared();
    slot.gameModeActive = false;
    slot.sharedHandle   = false;
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
    bool    loggedShape = false;
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
                // Payload is [chargeState, percent] on every transport, not
                // the [percent, chargeState] originally assumed. Confirmed on
                // the dongle too: reports read 1/92 and 1/91 — a constant 1
                // (CHARGE_STATE_DISCHARGING) followed by the real level.
                const uint8_t chargeState = buf[1];
                const uint8_t percent     = buf[2];
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

        // Report shape, once per read loop. The trackpad mouse needs a longer
        // report than the haptics do, so a transport that reports short loses
        // cursor movement while every buzz still fires — the same thing a
        // blocked SendInput looks like from the user's side, and only this
        // line tells them apart.
        if (!loggedShape) {
            loggedShape = true;
            EventLog::Write("REPORT: state id=0x%02X len=%zu (transport=%s)",
                            buf[0], n,
                            SteamController::TransportName(slot->transport));
            if (n < 30)
                EventLog::Write("REPORT: %zu bytes is under the 30 the trackpad "
                                "mouse requires — trackpad movement is being "
                                "dropped before it reaches SendInput", n);
        }

        // Whether each pad is pressed has to be settled before the pads are
        // updated, because a directional pad resolves its directions from it.
        {
            const uint8_t pb2 = n > 4 ? buf[4] : 0;
            const uint8_t pb3 = n > 5 ? buf[5] : 0;
            uint16_t pLArea = 0, pRArea = 0;
            if (n >= 30) {
                memcpy(&pLArea, buf + 22, 2);
                memcpy(&pRArea, buf + 28, 2);
            }
            slot->leftPress.Update(pLArea,
                (pb3 & SteamController::BTN_TP_LT_CLICK) != 0);
            slot->rightPress.Update(pRArea,
                (pb2 & SteamController::BTN_TP_RT_CLICK) != 0);
        }

        // The pads first: a directional pad's directions are resolved here and
        // the virtual controller is handed them, so updating it first would
        // report last frame's directions — a frame of lag on every press and,
        // worse, a direction still held for a frame after release.
        slot->leftPad.Update(buf, n, slot->leftPress.pressed);
        slot->rightPad.Update(buf, n, slot->rightPress.pressed);
        // Was that contact a tap? Only the lift can say — see Slot::TapState.
        const uint8_t tb2 = n > 4 ? buf[4] : 0;
        const uint8_t tb3 = n > 5 ? buf[5] : 0;
        int16_t tlx = 0, tly = 0, trx = 0, try_ = 0;
        if (n >= 28) {
            memcpy(&tlx,  buf + 18, 2);
            memcpy(&tly,  buf + 20, 2);
            memcpy(&trx,  buf + 24, 2);
            memcpy(&try_, buf + 26, 2);
        }

        const PadDigital resolved{
            slot->leftPad.Directions(),
            slot->rightPad.Directions(),
            slot->leftPad.ClickInCentre(),
            slot->rightPad.ClickInCentre(),
            // A tap is a contact that never became a press, and "became a
            // press" has to mean the same thing here as everywhere else. Asked
            // of the firmware's click bit instead, a gentle press it failed to
            // report would leave the contact looking like a tap, and firing
            // the tap binding on lift on top of the direction the press had
            // already sent.
            slot->leftTap.Update((tb3 & SteamController::BTN_TP_LT) != 0,
                                 slot->leftPress.pressed, tlx, tly),
            slot->rightTap.Update((tb2 & SteamController::BTN_TP_RT) != 0,
                                  slot->rightPress.pressed, trx, try_),
            slot->leftPress.pressed,
            slot->rightPress.pressed,
        };
        if (slot->vc) slot->vc->Update(buf, n, m_profile, resolved);

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
            // The press, from contact area rather than the firmware's click
            // bit. Feeding the latch the bit meant the haptic fired for fewer
            // than half the presses a thumb made, which is what "the pad only
            // buzzes sometimes" was.
            const bool rc = resolved.rightPressed;
            const bool lc = resolved.leftPressed;

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

            // Click haptic latch — both edges believe the firmware click bit
            // once it has held an answer long enough to not be chatter. See
            // the constants on Slot for why release stopped waiting on contact
            // area: a thumb that stays on the pad between rapid taps never
            // reaches idle, so every tap after the first went unfelt.
            slot->hapticRightClickTrueFrames = rc ? slot->hapticRightClickTrueFrames + 1 : 0;
            slot->hapticLeftClickTrueFrames  = lc ? slot->hapticLeftClickTrueFrames  + 1 : 0;
            slot->hapticRightClickLowFrames  = rc ? 0 : slot->hapticRightClickLowFrames + 1;
            slot->hapticLeftClickLowFrames   = lc ? 0 : slot->hapticLeftClickLowFrames  + 1;

            if (slot->hapticRightClickState == Slot::ClickState::WaitingForPress) {
                if (slot->hapticRightClickTrueFrames >= Slot::kPressConfirmFrames) {
                    slot->sc->PulseTrackpadHaptic(false, true);
                    slot->hapticRightClickState   = Slot::ClickState::WaitingForRelease;
                    slot->hapticPrevRightX        = rx;
                    slot->hapticPrevRightY        = ry;
                    slot->hapticRightDistAccum    = 0.0f;
                }
            } else if (slot->hapticRightClickLowFrames >= Slot::kReleaseConfirmFrames) {
                slot->sc->PulseTrackpadHaptic(false, true);
                slot->hapticRightClickState   = Slot::ClickState::WaitingForPress;
                slot->hapticRightReleaseGrace = Slot::kPostReleaseGraceFrames;
                slot->hapticPrevRightX        = rx;
                slot->hapticPrevRightY        = ry;
                slot->hapticRightDistAccum    = 0.0f;
            }

            if (slot->hapticLeftClickState == Slot::ClickState::WaitingForPress) {
                if (slot->hapticLeftClickTrueFrames >= Slot::kPressConfirmFrames) {
                    slot->sc->PulseTrackpadHaptic(true, true);
                    slot->hapticLeftClickState   = Slot::ClickState::WaitingForRelease;
                    slot->hapticPrevLeftX        = lx;
                    slot->hapticPrevLeftY        = ly;
                    slot->hapticLeftDistAccum    = 0.0f;
                }
            } else if (slot->hapticLeftClickLowFrames >= Slot::kReleaseConfirmFrames) {
                slot->sc->PulseTrackpadHaptic(true, true);
                slot->hapticLeftClickState   = Slot::ClickState::WaitingForPress;
                slot->hapticLeftReleaseGrace = Slot::kPostReleaseGraceFrames;
                slot->hapticPrevLeftX        = lx;
                slot->hapticPrevLeftY        = ly;
                slot->hapticLeftDistAccum    = 0.0f;
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

                // What a movement tick means depends on what the pad is for.
                //
                // A directional pad ticks once per direction change, which is
                // the detent a d-pad gives when the thumb rolls from one
                // direction to the next. Distance means nothing to it: sliding
                // around inside a single direction changes nothing, and would
                // buzz continuously for no event at all.
                //
                // Deliberately outside the distance gates below, all of which
                // would suppress it — directions only change while the click
                // is held, and those gates exist to keep a press from bleeding
                // a tick into the click haptic.
                //
                // Only between two directions: coming from nothing is the
                // press and going back to nothing is the release, and both
                // already fire their own click haptic.
                auto directionTick = [&](bool leftPad, uint8_t prev, uint8_t now) {
                    if (prev != DirNone && now != DirNone && prev != now)
                        slot->sc->TickTrackpadMovement(leftPad);
                };
                const bool rightIsDpad = m_profile.rightPad.IsDirectionalPad();
                const bool leftIsDpad  = m_profile.leftPad.IsDirectionalPad();
                if (rightIsDpad)
                    directionTick(false, slot->prevRightDirs, resolved.rightDirs);
                if (leftIsDpad)
                    directionTick(true, slot->prevLeftDirs, resolved.leftDirs);

                // A single button has no movement to report on at all, and a
                // directional pad has already had its say above.
                const bool rightWantsDistance =
                    !rightIsDpad && m_profile.rightPad.mode != TrackpadMode::SingleButton;
                const bool leftWantsDistance =
                    !leftIsDpad && m_profile.leftPad.mode != TrackpadMode::SingleButton;

                // Gate on the latch state, not the raw click bit — while the
                // click is held (even if the bit chatters low), no ticks.
                if (rightWantsDistance && rt && slot->hapticWasRightTouching && !rc
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
                if (leftWantsDistance && lt && slot->hapticWasLeftTouching && !lc
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

        // Deliver back paddles and pad clicks bound to a key or a mouse button.
        // These go out through SendInput rather than the virtual pad.
        // Edge-detected so each press sends exactly one down and each release
        // exactly one up.
        //
        // Pad clicks ride this same path rather than living in TrackpadInput,
        // which is what makes a pad's click independent of its movement mode —
        // the click used to be hardcoded to a left button and only fired while
        // that pad was driving the mouse.
        if (hasPrev) {
            // A pad set to feed the DS4 touchpad has no click binding to
            // dispatch — its press is the touchpad press. EffectiveClick
            // returns nothing in that case, so a binding left behind by an
            // earlier mode cannot fire from behind the hidden UI.
            const BackButtonBinding leftPadClick  = m_profile.leftPad.EffectiveClick();
            const BackButtonBinding rightPadClick = m_profile.rightPad.EffectiveClick();

            // A directional pad's click is only its click binding when the
            // press landed in the middle — out in the ring the same press is a
            // direction, and letting both through is the one physical press
            // meaning two things that the zone split exists to prevent. The
            // zone is latched for the press's whole life, so this cannot flip
            // mid-hold and strand a binding down. Every other mode answers
            // "centre", so they gate to exactly what they did before.
            const bool leftClickInCentre  = resolved.leftClickInCentre;
            const bool rightClickInCentre = resolved.rightClickInCentre;

            const uint8_t lDirs = resolved.leftDirs,  lPrevDirs = slot->prevLeftDirs;
            const uint8_t rDirs = resolved.rightDirs, rPrevDirs = slot->prevRightDirs;
            const auto& lPad = m_profile.leftPad;
            const auto& rPad = m_profile.rightPad;

            // Held by value rather than by reference: half of these bindings
            // are returned by value from the Effective* accessors, and a
            // reference member would be bound to a temporary.
            struct PaddleEdge { bool cur; bool prev; BackButtonBinding binding; };
            const PaddleEdge edges[] = {
                { n>4 && (buf[4]&SteamController::BTN_L4)!=0, (prevBuf[4]&SteamController::BTN_L4)!=0, m_profile.back.l4 },
                { n>4 && (buf[4]&SteamController::BTN_L5)!=0, (prevBuf[4]&SteamController::BTN_L5)!=0, m_profile.back.l5 },
                { n>2 && (buf[2]&SteamController::BTN_R4)!=0, (prevBuf[2]&SteamController::BTN_R4)!=0, m_profile.back.r4 },
                { n>3 && (buf[3]&SteamController::BTN_R5)!=0, (prevBuf[3]&SteamController::BTN_R5)!=0, m_profile.back.r5 },
                // Pressed comes from contact area, not the report's click bit,
                // so like the directions its previous answer is carried on the
                // slot rather than re-read from the previous report.
                { resolved.leftPressed  && leftClickInCentre,  slot->prevLeftPressed  && leftClickInCentre,  leftPadClick },
                { resolved.rightPressed && rightClickInCentre, slot->prevRightPressed && rightClickInCentre, rightPadClick },
                // A tap is not zoned — it cannot collide with a direction,
                // which comes from the click, and a thumb crosses zones
                // constantly while sliding. It is decided on the lift rather
                // than read from the report, though, so like the directions
                // its previous answer is carried on the slot.
                { resolved.leftTap,  slot->prevLeftTap,  lPad.EffectiveTouch() },
                { resolved.rightTap, slot->prevRightTap, rPad.EffectiveTouch() },
                // Directions are resolved rather than read, so their previous
                // state is the one carried on the slot, not a bit in prevBuf.
                { (lDirs&DirUp)!=0,    (lPrevDirs&DirUp)!=0,    lPad.EffectiveDirection(DirUp) },
                { (lDirs&DirDown)!=0,  (lPrevDirs&DirDown)!=0,  lPad.EffectiveDirection(DirDown) },
                { (lDirs&DirLeft)!=0,  (lPrevDirs&DirLeft)!=0,  lPad.EffectiveDirection(DirLeft) },
                { (lDirs&DirRight)!=0, (lPrevDirs&DirRight)!=0, lPad.EffectiveDirection(DirRight) },
                { (rDirs&DirUp)!=0,    (rPrevDirs&DirUp)!=0,    rPad.EffectiveDirection(DirUp) },
                { (rDirs&DirDown)!=0,  (rPrevDirs&DirDown)!=0,  rPad.EffectiveDirection(DirDown) },
                { (rDirs&DirLeft)!=0,  (rPrevDirs&DirLeft)!=0,  rPad.EffectiveDirection(DirLeft) },
                { (rDirs&DirRight)!=0, (rPrevDirs&DirRight)!=0, rPad.EffectiveDirection(DirRight) },
            };
            static_assert(std::size(edges) == Slot::kBindableCount,
                          "paddleHeld must have one entry per edge-dispatched binding");
            for (size_t i = 0; i < std::size(edges); ++i) {
                const auto& e = edges[i];
                if (e.cur == e.prev) continue;
                if (e.cur) {
                    if (SendPaddleInput(e.binding, true)) {
                        slot->paddleHeld[i]     = e.binding;
                        slot->paddleRepeatGap[i] = KeyRepeatInterval();
                        slot->paddleRepeatAt[i]  =
                            std::chrono::steady_clock::now() + KeyRepeatDelay();
                    }
                } else {
                    // Release what the press sent, not what the binding says
                    // now — the two differ if the user rebound mid-hold.
                    SendPaddleInput(slot->paddleHeld[i], false);
                    slot->paddleHeld[i] = BackButtonBinding{};
                }
            }
        }

        // Auto-repeat held keys. A physical keyboard repeats because its own
        // firmware keeps sending the key, not because Windows tracks the key as
        // down, so injected input has to reproduce that itself. Keys only —
        // mice do not repeat when held, and gamepad bindings are held state in
        // the pad report, where a repeat would be meaningless.
        for (size_t i = 0; i < std::size(slot->paddleHeld); ++i) {
            const auto& held = slot->paddleHeld[i];
            if (held.kind != BackButtonBinding::Kind::Key) continue;

            const auto now = std::chrono::steady_clock::now();
            if (now < slot->paddleRepeatAt[i]) continue;

            // The base key only. Modifiers stay held across the repeat, which
            // is exactly what a real keyboard does — holding Ctrl+K repeats
            // the K, it does not re-press Ctrl.
            SendKeyInput(held.code, true);
            slot->paddleRepeatAt[i] = now + slot->paddleRepeatGap[i];
        }

        // Button capture for the remap window (press-to-bind).
        // Only fires on a rising edge to avoid repeat-triggering on hold.
        if (m_capturing.load() && hasPrev) {
            BackButtonAction captured;
            if (DetectCapture(buf, prevBuf, captured)) {
                m_capturing = false;
                std::lock_guard<std::mutex> lk(m_captureMutex);
                if (m_captureCallback)
                    m_captureCallback(BackButtonBinding::FromAction(captured));
            }
        }

        // Deliberately outside the hasPrev guard above, alongside the report
        // it is the equivalent of: the first frame of a read loop establishes
        // a baseline rather than dispatching against a stale one, and every
        // later frame leaves behind what the next will compare with. Skipping
        // it on any frame would stand a direction up or down for good.
        slot->prevLeftDirs  = resolved.leftDirs;
        slot->prevRightDirs = resolved.rightDirs;
        slot->prevLeftTap  = resolved.leftTap;
        slot->prevRightTap = resolved.rightTap;
        slot->prevLeftPressed  = resolved.leftPressed;
        slot->prevRightPressed = resolved.rightPressed;

        memcpy(prevBuf, buf, n);
        hasPrev = true;
    }
}

void ControllerManager::NotifyStateChanged(bool padUnavailable) {
    m_onStateChanged(!m_slots.empty(), IsGameModeActive(), IsGameModeShared(), padUnavailable);
}
