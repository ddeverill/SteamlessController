#include "LinuxVirtualGamepad.h"
#include "core/SteamController.h"
#include "core/TrackpadConfig.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

// linux/input-event-codes.h #defines BTN_A/BTN_B/BTN_X/BTN_Y/BTN_DPAD_UP as
// aliases (BTN_SOUTH/EAST/NORTH/WEST and 0x220), which collide textually
// with SteamController's own same-named bitmask constants used throughout
// this file (e.g. `SteamController::BTN_A` preprocesses into nonsense once
// BTN_A is a macro). Capture the evdev values under different names before
// undefining the macros, so both can coexist in one translation unit.
namespace evdev {
constexpr uint16_t kBtnA = BTN_A;
constexpr uint16_t kBtnB = BTN_B;
constexpr uint16_t kBtnX = BTN_X;
constexpr uint16_t kBtnY = BTN_Y;
}  // namespace evdev
#undef BTN_A
#undef BTN_B
#undef BTN_X
#undef BTN_Y
#undef BTN_DPAD_UP

namespace {

void EmitEvent(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev{};
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    // Best-effort: a failed write here means the uinput device vanished
    // (the slot's controller unplugged, or the daemon is shutting down) —
    // there is nothing more useful to do than drop the event.
    if (write(fd, &ev, sizeof(ev)) < 0) { /* ignored, see above */ }
}

void SynReport(int fd) { EmitEvent(fd, EV_SYN, SYN_REPORT, 0); }

void AbsSetup(int fd, uint16_t code, int32_t min, int32_t max, int32_t fuzz, int32_t flat) {
    struct uinput_abs_setup s{};
    s.code           = code;
    s.absinfo.minimum = min;
    s.absinfo.maximum = max;
    s.absinfo.fuzz    = fuzz;
    s.absinfo.flat    = flat;
    ioctl(fd, UI_ABS_SETUP, &s);
}

// Legacy BTN_A/B/X/Y names, not BTN_SOUTH/EAST/NORTH/WEST: they are
// numerically identical, but xpad (the reference Xbox-360 uinput layout
// every SDL/Steam Input mapping is written against) sets exactly these
// four codes and skips BTN_C — using the "modern" aliases and reasoning by
// physical position (north = Y) swaps X and Y in every game.
constexpr uint16_t kButtonCodes[] = {
    evdev::kBtnA, evdev::kBtnB, evdev::kBtnX, evdev::kBtnY, BTN_TL, BTN_TR,
    BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR,
};

// Keys and mouse buttons are delivered by ControllerManager via the input
// injector, not through the virtual pad, so they fall through as "no
// gamepad input" here, same as on Windows.
struct PadFrame {
    bool a = false, b = false, x = false, y = false;
    bool lb = false, rb = false, back = false, start = false, guide = false;
    bool thumbL = false, thumbR = false;
    int  hatX = 0, hatY = 0;  // -1/0/1
    uint8_t triggerL = 0, triggerR = 0;
    int16_t stickLX = 0, stickLY = 0, stickRX = 0, stickRY = 0;
};

void ApplyBackAction(const BackButtonBinding& binding, PadFrame& r) {
    switch (binding.AsAction()) {
    case BackButtonAction::A:         r.a = true;     break;
    case BackButtonAction::B:         r.b = true;     break;
    case BackButtonAction::X:         r.x = true;     break;
    case BackButtonAction::Y:         r.y = true;     break;
    case BackButtonAction::LB:        r.lb = true;    break;
    case BackButtonAction::RB:        r.rb = true;    break;
    case BackButtonAction::LT:        r.triggerL = 255; break;
    case BackButtonAction::RT:        r.triggerR = 255; break;
    case BackButtonAction::DPadUp:    r.hatY = -1;    break;
    case BackButtonAction::DPadDown:  r.hatY = 1;     break;
    case BackButtonAction::DPadLeft:  r.hatX = -1;    break;
    case BackButtonAction::DPadRight: r.hatX = 1;     break;
    case BackButtonAction::Menu:      r.start = true; break;
    case BackButtonAction::View:      r.back = true;  break;
    case BackButtonAction::L3:        r.thumbL = true; break;
    case BackButtonAction::R3:        r.thumbR = true; break;
    default: break;
    }
}

PadFrame Translate(const uint8_t* buf, size_t n) {
    PadFrame r;
    if (n < 18) return r;

    const uint8_t b0 = buf[2];
    const uint8_t b1 = buf[3];
    const uint8_t b2 = buf[4];

    r.a = (b0 & SteamController::BTN_A) != 0;
    r.b = (b0 & SteamController::BTN_B) != 0;
    r.x = (b0 & SteamController::BTN_X) != 0;
    r.y = (b0 & SteamController::BTN_Y) != 0;
    r.lb = (b2 & SteamController::BTN_LB) != 0;
    r.rb = (b1 & SteamController::BTN_RB) != 0;
    r.start = (b0 & SteamController::BTN_MENU) != 0;
    r.back  = (b1 & SteamController::BTN_VIEW) != 0;
    r.thumbL = (b1 & SteamController::BTN_LS) != 0;
    r.thumbR = (b0 & SteamController::BTN_RS) != 0;
    r.guide  = (b2 & SteamController::BTN_STEAM) != 0;

    if (b1 & SteamController::BTN_DPAD_UP) r.hatY = -1;
    if (b1 & SteamController::BTN_DPAD_DN) r.hatY = 1;
    if (b1 & SteamController::BTN_DPAD_LT) r.hatX = -1;
    if (b1 & SteamController::BTN_DPAD_RT) r.hatX = 1;

    int16_t ltRaw, rtRaw;
    memcpy(&ltRaw, buf + 6, 2);
    memcpy(&rtRaw, buf + 8, 2);
    r.triggerL = static_cast<uint8_t>(std::clamp<int>(ltRaw >> 7, 0, 255));
    r.triggerR = static_cast<uint8_t>(std::clamp<int>(rtRaw >> 7, 0, 255));

    memcpy(&r.stickLX, buf + 10, 2);
    memcpy(&r.stickLY, buf + 12, 2);
    memcpy(&r.stickRX, buf + 14, 2);
    memcpy(&r.stickRY, buf + 16, 2);

    return r;
}

int16_t NegateAxis(int16_t v) {
    // evdev's Y grows downward; the Steam Controller's reports grow upward.
    return static_cast<int16_t>(v == -32768 ? 32767 : -v);
}

}  // namespace

LinuxVirtualGamepad::LinuxVirtualGamepad(ControllerPlatform platform, RumbleCallback rumbleCallback)
    : m_platform(platform), m_rumbleCallback(std::move(rumbleCallback))
{
    m_fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) {
        m_failStage     = "open(/dev/uinput)";
        m_driverMissing = (errno == ENOENT);
        m_busReport     = errno == ENOENT
            ? "uinput module not loaded — run: sudo modprobe uinput"
            : (errno == EACCES
                ? "no permission on /dev/uinput — install the udev rules and "
                  "run: sudo udevadm control --reload-rules && sudo udevadm trigger"
                : strerror(errno));
        return;
    }

    ioctl(m_fd, UI_SET_EVBIT, EV_KEY);
    for (uint16_t code : kButtonCodes) ioctl(m_fd, UI_SET_KEYBIT, code);

    ioctl(m_fd, UI_SET_EVBIT, EV_ABS);
    AbsSetup(m_fd, ABS_X,  -32768, 32767, 16, 128);
    AbsSetup(m_fd, ABS_Y,  -32768, 32767, 16, 128);
    AbsSetup(m_fd, ABS_RX, -32768, 32767, 16, 128);
    AbsSetup(m_fd, ABS_RY, -32768, 32767, 16, 128);
    AbsSetup(m_fd, ABS_Z,       0,   255,  0,   0);   // left trigger
    AbsSetup(m_fd, ABS_RZ,      0,   255,  0,   0);   // right trigger
    AbsSetup(m_fd, ABS_HAT0X,  -1,     1,  0,   0);
    AbsSetup(m_fd, ABS_HAT0Y,  -1,     1,  0,   0);

    ioctl(m_fd, UI_SET_EVBIT, EV_FF);
    ioctl(m_fd, UI_SET_FFBIT, FF_RUMBLE);

    struct uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor  = 0x045e;  // Microsoft
    setup.id.product = 0x028e;  // Xbox 360 wired pad — matches SDL2/Steam Input's built-in mapping
    setup.id.version = 0x0110;
    setup.ff_effects_max = 16;  // must be > 0 or the kernel never routes FF requests to us
    std::snprintf(setup.name, sizeof(setup.name), "Microsoft X-Box 360 pad");

    if (ioctl(m_fd, UI_DEV_SETUP, &setup) < 0) {
        m_failStage = "UI_DEV_SETUP";
        m_lastError = static_cast<uint32_t>(errno);
        close(m_fd); m_fd = -1;
        return;
    }
    if (ioctl(m_fd, UI_DEV_CREATE) < 0) {
        m_failStage = "UI_DEV_CREATE";
        m_lastError = static_cast<uint32_t>(errno);
        close(m_fd); m_fd = -1;
        return;
    }

    m_ffRunning = true;
    m_ffThread  = std::thread(&LinuxVirtualGamepad::FfLoop, this);

    m_valid = true;
    printf("[uinput] Virtual Xbox 360 pad created (%s requested)\n",
           platform == ControllerPlatform::PlayStation ? "PlayStation" : "Xbox");
}

LinuxVirtualGamepad::~LinuxVirtualGamepad() {
    m_ffRunning = false;
    if (m_fd >= 0) {
        // Wake the blocking read in FfLoop by tearing the device down first;
        // UI_DEV_DESTROY causes any pending read() on this fd to return.
        ioctl(m_fd, UI_DEV_DESTROY);
    }
    if (m_ffThread.joinable()) m_ffThread.join();
    if (m_stopThread.joinable()) m_stopThread.join();
    if (m_fd >= 0) close(m_fd);
}

void LinuxVirtualGamepad::FfLoop() {
    while (m_ffRunning) {
        struct pollfd pfd{ m_fd, POLLIN, 0 };
        if (poll(&pfd, 1, 200) <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        struct input_event ev{};
        if (read(m_fd, &ev, sizeof(ev)) != static_cast<ssize_t>(sizeof(ev))) continue;

        if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD) {
            HandleFfUpload();
        } else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE) {
            HandleFfErase();
        } else if (ev.type == EV_FF) {
            if (ev.code == FF_GAIN) continue;  // gain scaling not modeled; SDL rarely sets it
            HandlePlay(ev.code, ev.value);
        }
    }
}

void LinuxVirtualGamepad::HandleFfUpload() {
    struct uinput_ff_upload up{};
    // request_id is read via UI_BEGIN_FF_UPLOAD, not the EV_UINPUT event value.
    if (ioctl(m_fd, UI_BEGIN_FF_UPLOAD, &up) < 0) return;

    if (up.effect.type == FF_RUMBLE) {
        m_effect.strong   = up.effect.u.rumble.strong_magnitude;
        m_effect.weak     = up.effect.u.rumble.weak_magnitude;
        m_effect.lengthMs = up.effect.replay.length;
        m_haveEffect      = true;
        up.retval = 0;
    } else {
        up.retval = -1;  // only FF_RUMBLE is advertised/expected
    }
    ioctl(m_fd, UI_END_FF_UPLOAD, &up);
}

void LinuxVirtualGamepad::HandleFfErase() {
    struct uinput_ff_erase er{};
    if (ioctl(m_fd, UI_BEGIN_FF_ERASE, &er) < 0) return;
    m_haveEffect = false;
    er.retval = 0;
    ioctl(m_fd, UI_END_FF_ERASE, &er);
}

void LinuxVirtualGamepad::HandlePlay(int /*effectId*/, int repeatCount) {
    if (repeatCount == 0 || !m_haveEffect) {
        if (m_rumbleCallback) m_rumbleCallback(0, 0);
        return;
    }
    // SDL uploads magnitudes as full 16-bit values; SteamController::SetRumble
    // wants the 8-bit XInput-shaped motor bytes.
    const uint8_t large = static_cast<uint8_t>(m_effect.strong >> 8);
    const uint8_t small = static_cast<uint8_t>(m_effect.weak   >> 8);
    if (m_rumbleCallback) m_rumbleCallback(large, small);

    // evdev FF effects carry a duration the kernel does not enforce for a
    // uinput passthrough device — arm a one-shot timer to stop the motors,
    // the same way ViGEm's level-based rumble never needed one.
    const uint64_t generation = ++m_playGeneration;
    const uint32_t durationMs = m_effect.lengthMs;
    if (durationMs == 0) return;  // 0 conventionally means "play until told otherwise"

    if (m_stopThread.joinable()) m_stopThread.join();
    m_stopThread = std::thread([this, generation, durationMs] {
        std::this_thread::sleep_for(std::chrono::milliseconds(durationMs));
        if (m_playGeneration.load() == generation && m_rumbleCallback)
            m_rumbleCallback(0, 0);
    });
}

void LinuxVirtualGamepad::SetBatteryState(uint8_t, uint8_t) {
    // No DS4 target on Linux to report battery through; see the class comment.
}

void LinuxVirtualGamepad::Update(const uint8_t* buf, size_t n, const ControllerProfile& profile) {
    if (!m_valid) return;
    const BackButtonConfig& backCfg = profile.back;

    PadFrame r = Translate(buf, n);

    if (n > 4) {
        if (buf[4] & SteamController::BTN_L4) ApplyBackAction(backCfg.l4, r);
        if (buf[4] & SteamController::BTN_L5) ApplyBackAction(backCfg.l5, r);
    }
    if (n > 2 && (buf[2] & SteamController::BTN_R4)) ApplyBackAction(backCfg.r4, r);
    if (n > 3 && (buf[3] & SteamController::BTN_R5)) ApplyBackAction(backCfg.r5, r);
    // Pad clicks bound to a gamepad action reach the virtual pad the same
    // way the paddles do. EffectiveClick yields nothing for a pad feeding
    // the (Windows-only) DS4 touchpad, which has no Linux equivalent anyway.
    if (n > 5 && (buf[5] & SteamController::BTN_TP_LT_CLICK)) ApplyBackAction(profile.leftPad.EffectiveClick(), r);
    if (n > 4 && (buf[4] & SteamController::BTN_TP_RT_CLICK)) ApplyBackAction(profile.rightPad.EffectiveClick(), r);

    EmitEvent(m_fd, EV_KEY, evdev::kBtnA, r.a);
    EmitEvent(m_fd, EV_KEY, evdev::kBtnB, r.b);
    EmitEvent(m_fd, EV_KEY, evdev::kBtnX, r.x);
    EmitEvent(m_fd, EV_KEY, evdev::kBtnY, r.y);
    EmitEvent(m_fd, EV_KEY, BTN_TL, r.lb);
    EmitEvent(m_fd, EV_KEY, BTN_TR, r.rb);
    EmitEvent(m_fd, EV_KEY, BTN_SELECT, r.back);
    EmitEvent(m_fd, EV_KEY, BTN_START, r.start);
    EmitEvent(m_fd, EV_KEY, BTN_MODE, r.guide);
    EmitEvent(m_fd, EV_KEY, BTN_THUMBL, r.thumbL);
    EmitEvent(m_fd, EV_KEY, BTN_THUMBR, r.thumbR);

    EmitEvent(m_fd, EV_ABS, ABS_HAT0X, r.hatX);
    EmitEvent(m_fd, EV_ABS, ABS_HAT0Y, r.hatY);
    EmitEvent(m_fd, EV_ABS, ABS_Z,  r.triggerL);
    EmitEvent(m_fd, EV_ABS, ABS_RZ, r.triggerR);
    EmitEvent(m_fd, EV_ABS, ABS_X,  r.stickLX);
    EmitEvent(m_fd, EV_ABS, ABS_Y,  NegateAxis(r.stickLY));
    EmitEvent(m_fd, EV_ABS, ABS_RX, r.stickRX);
    EmitEvent(m_fd, EV_ABS, ABS_RY, NegateAxis(r.stickRY));

    SynReport(m_fd);
}

// ---------------------------------------------------------------------------
// LinuxVirtualGamepadFactory
// ---------------------------------------------------------------------------

std::unique_ptr<IVirtualGamepad> LinuxVirtualGamepadFactory::Create(
        ControllerPlatform platform, RumbleCallback rumbleCallback) {
    return std::make_unique<LinuxVirtualGamepad>(platform, std::move(rumbleCallback));
}

bool LinuxVirtualGamepadFactory::BusAvailable() const {
    return access("/dev/uinput", W_OK) == 0;
}

std::string LinuxVirtualGamepadFactory::BusReport() const {
    struct stat st{};
    if (stat("/dev/uinput", &st) != 0)
        return "uinput module not loaded — run: sudo modprobe uinput";
    if (access("/dev/uinput", W_OK) != 0)
        return "no permission on /dev/uinput — install the udev rules and "
               "run: sudo udevadm control --reload-rules && sudo udevadm trigger";
    return "/dev/uinput present and writable";
}
