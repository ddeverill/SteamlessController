#include "ControllerManager.h"
#include "EventLog.h"
#include "SteamController.h"
#include "TrackpadMouse.h"
#include "iface/IDeviceReclaimer.h"
#include "iface/IHidDevice.h"
#include "iface/IInputInjector.h"
#include "iface/IOnScreenKeyboard.h"
#include "iface/IPlatform.h"
#include "iface/IVirtualGamepad.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <thread>
#include <utility>

// ---------------------------------------------------------------------------
// Slot
// ---------------------------------------------------------------------------

struct ControllerManager::Slot {
    explicit Slot(IInputInjector& injector) : leftPad(injector), rightPad(injector) {}
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;

    std::string                        path;
    SteamController::Transport         transport = SteamController::Transport::Unknown;
    std::unique_ptr<SteamController>   sc;
    std::unique_ptr<IVirtualGamepad>   vc;
    // One per physical pad — they are configured independently.
    TrackpadMouse                      leftPad;
    TrackpadMouse                      rightPad;
    std::thread                        readThread;
    std::atomic<bool>                  readRunning{false};
    bool                               gameModeActive = false;
    int                                lastBatteryPercent = -1;  // -1 = never reported

    // When this slot was last found silent. Probing for a state report costs
    // the full timeout on an empty puck slot, and the acquire path retries in
    // a rapid burst — without this, three empty slots would block the caller
    // for that timeout on every one of those attempts.
    std::chrono::steady_clock::time_point lastSilentAt{};

    // What each bindable button is currently holding down, so leaving game
    // mode mid-press releases it instead of stranding the input down. Records
    // what the press actually sent rather than reading the binding again on
    // release, so rebinding mid-hold still releases cleanly.
    //
    // Indices 0-3 are the paddles (L4, L5, R4, R5); 4-5 are the left and right
    // pad clicks, which are ordinary bindings dispatched down this same path.
    static constexpr size_t kBindableCount = 6;
    BackButtonBinding paddleHeld[kBindableCount];

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
// Lifecycle
// ---------------------------------------------------------------------------

ControllerManager::ControllerManager(IPlatform& platform, StateChangedFn onStateChanged)
    : m_platform(platform)
    , m_modifiers(platform.Input())
    , m_onStateChanged(std::move(onStateChanged))
{
    // If the process dies with the controller in game mode, the user is left
    // without input until the firmware's own lizard revert timeout kicks in
    // (several seconds). The crash handler shortens that window by best-effort
    // sending the lizard restore reports on the way down.
    m_platform.InstallCrashHandler([this] { EmergencyRestoreAll(); });
    SyncDevices();
}

ControllerManager::~ControllerManager() {
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

void ControllerManager::OnDeviceChange() {
    SyncDevices();
}

ControllerManager::GameModeOutcome ControllerManager::EnableGameMode(uint32_t stateWaitMs) {
    // Once the absence of a bus/driver is established, re-establish it the
    // cheap way. Finding out by attempting an enable costs the user's
    // controller: each attempt claims the device exclusively and toggles
    // lizard mode off and back on, and the retry that waits for a driver to
    // be installed would otherwise do that every 30 seconds for as long as
    // the machine goes without one. Checking BusAvailable() touches no
    // controller device at all.
    if (m_lastPadDriverMissing && !m_platform.Gamepads().BusAvailable()) {
        NotifyStateChanged(true);
        return GameModeOutcome::VirtualPadUnavailable;
    }

    bool anyPadUnavailable = false;
    bool anyBlocked        = false;
    bool anyEnabled        = false;
    for (auto& slot : m_slots) {
        switch (EnableGameModeSlot(*slot, anyPadUnavailable, stateWaitMs)) {
        case GameModeOutcome::Enabled:               anyEnabled = true; break;
        case GameModeOutcome::Blocked:               anyBlocked = true; break;
        case GameModeOutcome::VirtualPadUnavailable: break;  // tracked by the out-param
        case GameModeOutcome::NoActiveController:    break;
        }
    }
    NotifyStateChanged(anyPadUnavailable);
    if (anyEnabled) return GameModeOutcome::Enabled;
    // Only report Blocked when something actually stood in the way. Slots that
    // are merely silent mean no controller is switched on, which no amount of
    // device cycling will change.
    //
    // Blocked outranks a missing pad: contention is the one of the two a
    // device cycle can still resolve, so if any slot is contested the caller
    // should hear about that rather than settle into the slow retry.
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
    if (!m_slots.empty())
        EventLog::Write("RELEASE: closing all device handles (intentional handoff)");
    // Disable game mode first — enables lizard mode and tears down the
    // virtual pad while we still hold write access to the device.
    DisableGameMode();
    // Close all device handles. Slot destructors call SteamController::Close()
    // which closes the HID handle, allowing another process to open it.
    m_slots.clear();
    NotifyStateChanged();
}

void ControllerManager::ApplyPadSettings(Slot& slot) {
    slot.leftPad.SetPad(true);
    slot.rightPad.SetPad(false);
    slot.leftPad.SetMode(m_profile.leftPad.mode);
    slot.rightPad.SetMode(m_profile.rightPad.mode);
    slot.leftPad.SetScrollDirection(m_profile.leftPad.scrollDir);
    slot.rightPad.SetScrollDirection(m_profile.rightPad.scrollDir);
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
        DisableGameMode();
        EnableGameMode();  // re-applies pad settings to each slot as it comes up
        return;
    }

    // Live update: the ReadLoop reads m_profile on every frame, so paddle and
    // pad-click changes take effect on the very next report. Movement modes
    // live inside the per-slot TrackpadMouse objects, so those are pushed.
    for (auto& slot : m_slots)
        ApplyPadSettings(*slot);
}

bool ControllerManager::SendPaddleInput(const BackButtonBinding& binding, bool down) {
    IInputInjector& input = m_platform.Input();

    switch (binding.kind) {
    case BackButtonBinding::Kind::Key:
        // Modifiers wrap the key: down before it so the application sees them
        // already held when the key arrives, up after it so the shortcut is
        // never briefly the unmodified key on the way out.
        if (down) {
            m_modifiers.Apply(binding.mods, true);
            input.Key(binding.code, true);
        } else {
            input.Key(binding.code, false);
            m_modifiers.Apply(binding.mods, false);
        }
        return true;

    case BackButtonBinding::Kind::MouseButton: {
        switch (static_cast<BackButtonBinding::MouseButtonCode>(binding.code)) {
        case BackButtonBinding::MouseButtonCode::Middle:
            input.Button(MouseButtonId::Middle, down);
            return true;
        case BackButtonBinding::MouseButtonCode::X1:
            input.Button(MouseButtonId::X1, down);
            return true;
        case BackButtonBinding::MouseButtonCode::X2:
            input.Button(MouseButtonId::X2, down);
            return true;
        default:
            return false;
        }
    }

    case BackButtonBinding::Kind::Action:
        if (binding.IsAction(BackButtonAction::LeftMouseButton)) {
            input.Button(MouseButtonId::Left, down);
            return true;
        }
        if (binding.IsAction(BackButtonAction::RightMouseButton)) {
            input.Button(MouseButtonId::Right, down);
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
            if (down) m_platform.Osk().Toggle();
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

void ControllerManager::SyncDevices() {
    auto liveDevices = SteamController::EnumerateAll(m_platform.Hid());

    auto it = m_slots.begin();
    while (it != m_slots.end()) {
        bool alive = std::any_of(liveDevices.begin(), liveDevices.end(),
            [&](const auto& info) { return info.path == (*it)->path; });
        if (!alive) {
            EventLog::Write("DISCONNECT: OS removed device (transport=%s, gameMode=%d, lastBattery=%d%%) %s",
                            SteamController::TransportName((*it)->transport),
                            (*it)->gameModeActive ? 1 : 0,
                            (*it)->lastBatteryPercent, (*it)->path.c_str());
            // Alert only when the controller was in active use; deliberate
            // unplugs while idle (and our own device cycles, which release
            // slots before cycling) stay quiet.
            if ((*it)->gameModeActive && m_alertFn) {
                if ((*it)->transport == SteamController::Transport::Bluetooth)
                    m_alertFn("Controller disconnected",
                              "The Bluetooth connection dropped — the controller "
                              "powered off, went to sleep, or is out of range. "
                              "Press the Steam button to reconnect.");
                else
                    m_alertFn("Controller disconnected",
                              "The USB device was removed. Check the cable or dongle — "
                              "if this happens randomly, USB power saving may be "
                              "suspending the port.");
            }
            DisableGameModeSlot(**it);
            it = m_slots.erase(it);
        } else {
            ++it;
        }
    }

    for (auto const& info : liveDevices) {
        bool already = std::any_of(m_slots.begin(), m_slots.end(),
            [&](const auto& s) { return s->path == info.path; });
        if (!already)
            OpenSlot(info);
    }

    NotifyStateChanged();
}

void ControllerManager::OpenSlot(const HidDeviceInfo& info) {
    auto slot = std::make_unique<Slot>(m_platform.Input());
    slot->path      = info.path;
    slot->transport = SteamController::TransportFrom(info);
    slot->sc        = std::make_unique<SteamController>(m_platform.Hid());
    if (!slot->sc->Open(info.path)) return;

    EventLog::Write("CONNECT: transport=%s %s",
                    SteamController::TransportName(slot->transport), info.path.c_str());

    m_slots.push_back(std::move(slot));

    if (IsGameModeActive()) {
        bool dummy = false;
        EnableGameModeSlot(*m_slots.back(), dummy, 250);
    } else {
        // Restore lizard mode in case a previous session crashed without cleaning up.
        m_slots.back()->sc->EnableLizardMode();
    }
}

// ---------------------------------------------------------------------------
// Game mode per-slot helpers
// ---------------------------------------------------------------------------

ControllerManager::GameModeOutcome
ControllerManager::EnableGameModeSlot(Slot& slot, bool& padUnavailableOut,
                                      uint32_t stateWaitMs) {
    if (slot.gameModeActive) return GameModeOutcome::Enabled;
    // The puck publishes a controller interface per slot and current firmware
    // rejects the lizard-mode command on an empty one, so only act on a slot
    // that is actually emitting state. Recently-silent slots are skipped
    // outright — see Slot::lastSilentAt.
    static constexpr auto kSilentSlotRetryGap = std::chrono::milliseconds(1500);
    const auto now = std::chrono::steady_clock::now();
    if (now - slot.lastSilentAt < kSilentSlotRetryGap)
        return GameModeOutcome::NoActiveController;

    if (!slot.sc->WaitForStateReport(stateWaitMs)) {
        slot.lastSilentAt = std::chrono::steady_clock::now();
        EventLog::Write("GAMEMODE: no state reports from slot (transport=%s), skipping %s",
                        SteamController::TransportName(slot.transport), slot.path.c_str());
        return GameModeOutcome::NoActiveController;
    }
    EventLog::Write("GAMEMODE: state report seen (transport=%s), proceeding %s",
                    SteamController::TransportName(slot.transport), slot.path.c_str());

    const auto claim = slot.sc->ClaimGameModeAccess();
    if (claim == SteamController::AccessClaim::Failed) {
        EventLog::Write("GAMEMODE: device reopen failed %s", slot.path.c_str());
        return GameModeOutcome::Blocked;
    }
    if (claim == SteamController::AccessClaim::Shared) {
        // Someone else holds a write handle. While Steam is running it is
        // most likely Steam's — but nothing here can tell one holder from
        // another, so the message says only what is actually known. Refusing
        // only makes sense where it leads somewhere: on a platform with a
        // working reclaimer, it is what escalates to a device cycle that
        // takes the handle back. Without one, refusing whenever Steam is
        // merely running would just permanently lock out every auto mode
        // built to hold the controller during a Steam game — the one thing
        // a reclaimer can't fix here, since hidraw's shared access was never
        // a lock to break in the first place. (On Linux this branch is the
        // only one that ever runs, and its reclaimer is never Available() —
        // see IHidDevice::Reopen and NullDeviceReclaimer.)
        if (m_steamPresent && m_platform.Reclaimer().Available()) {
            EventLog::Write("GAMEMODE: exclusive claim blocked — another process holds a "
                            "write handle (Steam running) %s",
                            slot.path.c_str());
            return GameModeOutcome::Blocked;
        }
        if (m_steamPresent) {
            // Proceeding deliberately: Steam is running and may still be
            // driving this same node via its own Steam Input if the user
            // hasn't disabled that for this controller — the write conflict
            // that risks is on them to avoid, not something refusing here
            // could have prevented anyway.
            EventLog::Write("GAMEMODE: Steam is running with no way to reclaim the device here; "
                            "proceeding on shared access — disable Steam Input for this "
                            "controller if it starts fighting Steam for it %s",
                            slot.path.c_str());
        } else {
            EventLog::Write("GAMEMODE: exclusive claim unavailable; using shared access %s",
                            slot.path.c_str());
        }
    }
    if (!slot.sc->DisableLizardMode()) {
        EventLog::Write("GAMEMODE: DisableLizardMode failed %s", slot.path.c_str());
        slot.sc->ReleaseToShared();
        return GameModeOutcome::Blocked;
    }

    slot.vc = m_platform.Gamepads().Create(
        m_profile.platform,
        [sc = slot.sc.get()](uint8_t largeMotor, uint8_t smallMotor) {
            if (sc->IsOpen()) sc->SetRumble(largeMotor, smallMotor);
        });
    if (!slot.vc->IsValid()) {
        EventLog::Write("GAMEMODE: virtual controller failed (stage=%s, err=0x%08X, driverMissing=%d)",
                        slot.vc->FailStage(), slot.vc->LastError(),
                        slot.vc->IsDriverMissing() ? 1 : 0);
        // The line above says a call failed; this one says what the machine
        // actually looks like. A second bus is the most likely cause of an
        // otherwise inexplicable connect failure and the hardest thing to
        // guess at from the outside, so it goes in the log every time.
        if (!slot.vc->BusReport().empty()) {
            EventLog::Write("GAMEMODE: %s", slot.vc->BusReport().c_str());
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

    EventLog::Write("GAMEMODE: enabled %s", slot.path.c_str());
    // Takeover is the moment users report the trackpad arriving dead, and what
    // decides whether injection can land is whatever window happens to be in
    // front right then — so record it here rather than reconstructing it later.
    if (m_profile.leftPad.ClaimedForDesktop() || m_profile.rightPad.ClaimedForDesktop())
        m_platform.Input().LogEnvironment("game mode taken");
    slot.gameModeActive = true;
    slot.leftPad.Reset();
    slot.rightPad.Reset();
    ReleaseHeldPaddleInputs(slot);
    ApplyPadSettings(slot);
    StartReadLoop(slot);
    return GameModeOutcome::Enabled;
}

void ControllerManager::DisableGameModeSlot(Slot& slot) {
    if (!slot.gameModeActive) return;
    EventLog::Write("GAMEMODE: disabled %s", slot.path.c_str());
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
}

// ---------------------------------------------------------------------------
// Crash-path lizard restore
// ---------------------------------------------------------------------------

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
                EventLog::Write("STALL: no reports for 4s (lastBattery=%d%%) %s",
                                slot->lastBatteryPercent, slot->path.c_str());
                if (m_alertFn) {
                    char text[256];
                    if (slot->lastBatteryPercent >= 0)
                        std::snprintf(text, sizeof(text),
                                   "No input received for several seconds — the battery may "
                                   "be dead, the controller may have gone to sleep, or the "
                                   "wireless signal dropped. Last battery report: %d%%.",
                                   slot->lastBatteryPercent);
                    else
                        std::snprintf(text, sizeof(text),
                                   "No input received for several seconds — the battery may "
                                   "be dead, the controller may have gone to sleep, or the "
                                   "wireless signal dropped.");
                    m_alertFn("Controller not responding", text);
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
        // blocked injection looks like from the user's side, and only this
        // line tells them apart.
        if (!loggedShape) {
            loggedShape = true;
            EventLog::Write("REPORT: state id=0x%02X len=%zu (transport=%s)",
                            buf[0], n,
                            SteamController::TransportName(slot->transport));
            if (n < 30)
                EventLog::Write("REPORT: %zu bytes is under the 30 the trackpad "
                                "mouse requires — trackpad movement is being "
                                "dropped before it reaches the input injector", n);
        }

        if (slot->vc) slot->vc->Update(buf, n, m_profile);
        slot->leftPad.Update(buf, n);
        slot->rightPad.Update(buf, n);

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

        // Deliver back paddles and pad clicks bound to a key or a mouse button.
        // These go out through the input injector rather than the virtual pad.
        // Edge-detected so each press sends exactly one down and each release
        // exactly one up.
        //
        // Pad clicks ride this same path rather than living in TrackpadMouse,
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

            struct PaddleEdge { bool cur; bool prev; const BackButtonBinding& binding; };
            const PaddleEdge edges[] = {
                { n>4 && (buf[4]&SteamController::BTN_L4)!=0, (prevBuf[4]&SteamController::BTN_L4)!=0, m_profile.back.l4 },
                { n>4 && (buf[4]&SteamController::BTN_L5)!=0, (prevBuf[4]&SteamController::BTN_L5)!=0, m_profile.back.l5 },
                { n>2 && (buf[2]&SteamController::BTN_R4)!=0, (prevBuf[2]&SteamController::BTN_R4)!=0, m_profile.back.r4 },
                { n>3 && (buf[3]&SteamController::BTN_R5)!=0, (prevBuf[3]&SteamController::BTN_R5)!=0, m_profile.back.r5 },
                { n>5 && (buf[5]&SteamController::BTN_TP_LT_CLICK)!=0, (prevBuf[5]&SteamController::BTN_TP_LT_CLICK)!=0, leftPadClick },
                { n>4 && (buf[4]&SteamController::BTN_TP_RT_CLICK)!=0, (prevBuf[4]&SteamController::BTN_TP_RT_CLICK)!=0, rightPadClick },
            };
            static_assert(std::size(edges) == Slot::kBindableCount,
                          "paddleHeld must have one entry per edge-dispatched binding");
            for (size_t i = 0; i < std::size(edges); ++i) {
                const auto& e = edges[i];
                if (e.cur == e.prev) continue;
                if (e.cur) {
                    if (SendPaddleInput(e.binding, true)) {
                        slot->paddleHeld[i]     = e.binding;
                        slot->paddleRepeatGap[i] = m_platform.Input().KeyRepeatInterval();
                        slot->paddleRepeatAt[i]  =
                            std::chrono::steady_clock::now() + m_platform.Input().KeyRepeatDelay();
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
        // firmware keeps sending the key, not because the OS tracks the key as
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
            m_platform.Input().Key(held.code, true);
            slot->paddleRepeatAt[i] = now + slot->paddleRepeatGap[i];
        }

        // Button capture for the remap UI/CLI (press-to-bind).
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

        memcpy(prevBuf, buf, n);
        hasPrev = true;
    }
}

void ControllerManager::NotifyStateChanged(bool padUnavailable) {
    m_onStateChanged(!m_slots.empty(), IsGameModeActive(), padUnavailable);
}
