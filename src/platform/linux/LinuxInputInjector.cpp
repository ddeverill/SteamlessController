#include "LinuxInputInjector.h"
#include "LinuxKeyMap.h"
#include "core/EventLog.h"
#include "core/KeyNames.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <unistd.h>
#include <sys/ioctl.h>

namespace {

void EmitEvent(int fd, uint16_t type, uint16_t code, int32_t value) {
    if (fd < 0) return;
    struct input_event ev{};
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    // Best-effort: a failed write here means the uinput device vanished
    // (unplugged/daemon shutting down) — there is nothing more useful to do
    // than drop the event, and the caller has no per-event result to check.
    if (write(fd, &ev, sizeof(ev)) < 0) { /* ignored, see above */ }
}

void SynReport(int fd) { EmitEvent(fd, EV_SYN, SYN_REPORT, 0); }

int OpenUinput() { return open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC); }

bool FinishDevice(int fd, struct uinput_setup& setup) {
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) return false;
    return ioctl(fd, UI_DEV_CREATE) >= 0;
}

}  // namespace

int LinuxInputInjector::CreateMouseDevice() {
    const int fd = OpenUinput();
    if (fd < 0) return -1;

    ioctl(fd, UI_SET_EVBIT, EV_REL);
    for (uint16_t code : { REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES })
        ioctl(fd, UI_SET_RELBIT, code);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (uint16_t code : { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA })
        ioctl(fd, UI_SET_KEYBIT, code);

    struct uinput_setup setup{};
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x28de;
    setup.id.product = 0xf001;
    setup.id.version = 1;
    std::snprintf(setup.name, sizeof(setup.name), "SteamlessController Mouse");

    if (!FinishDevice(fd, setup)) { close(fd); return -1; }
    return fd;
}

int LinuxInputInjector::CreateKeyboardDevice() {
    const int fd = OpenUinput();
    if (fd < 0) return -1;

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    // The whole bindable KEY_* range — same approach ydotool and similar
    // uinput-based tools use, rather than enumerating exactly what
    // LinuxKeyMap can produce.
    for (uint16_t code = 1; code < KEY_MAX; ++code)
        ioctl(fd, UI_SET_KEYBIT, code);

    struct uinput_setup setup{};
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x28de;
    setup.id.product = 0xf002;
    setup.id.version = 1;
    std::snprintf(setup.name, sizeof(setup.name), "SteamlessController Keyboard");

    if (!FinishDevice(fd, setup)) { close(fd); return -1; }
    return fd;
}

bool LinuxInputInjector::EnsureMouse() {
    if (m_mouseFd >= 0) return true;
    m_mouseFd = CreateMouseDevice();
    if (m_mouseFd < 0)
        EventLog::Write("INJECT: failed to create virtual mouse (uinput unavailable?)");
    return m_mouseFd >= 0;
}

bool LinuxInputInjector::EnsureKeyboard() {
    if (m_keyboardFd >= 0) return true;
    m_keyboardFd = CreateKeyboardDevice();
    if (m_keyboardFd < 0)
        EventLog::Write("INJECT: failed to create virtual keyboard (uinput unavailable?)");
    return m_keyboardFd >= 0;
}

LinuxInputInjector::~LinuxInputInjector() {
    if (m_mouseFd    >= 0) { ioctl(m_mouseFd,    UI_DEV_DESTROY); close(m_mouseFd); }
    if (m_keyboardFd >= 0) { ioctl(m_keyboardFd, UI_DEV_DESTROY); close(m_keyboardFd); }
}

bool LinuxInputInjector::MoveMouse(int dx, int dy) {
    if (!EnsureMouse()) return false;
    if (dx != 0) EmitEvent(m_mouseFd, EV_REL, REL_X, dx);
    if (dy != 0) EmitEvent(m_mouseFd, EV_REL, REL_Y, dy);
    SynReport(m_mouseFd);
    return true;
}

void LinuxInputInjector::Scroll(int vDelta, int hDelta) {
    if (!EnsureMouse()) return;
    // vDelta/hDelta arrive in the traditional 120-units-per-notch convention.
    // Modern libinput/GTK/Qt prefer the hi-res wheel axes (1/120 of a notch);
    // emit both so old and new listeners alike see smooth scrolling, in the
    // same SYN_REPORT frame.
    if (vDelta != 0) {
        EmitEvent(m_mouseFd, EV_REL, REL_WHEEL_HI_RES, vDelta);
        m_scrollAccumV += vDelta;
        while (m_scrollAccumV >= 120) { EmitEvent(m_mouseFd, EV_REL, REL_WHEEL, 1);  m_scrollAccumV -= 120; }
        while (m_scrollAccumV <= -120){ EmitEvent(m_mouseFd, EV_REL, REL_WHEEL, -1); m_scrollAccumV += 120; }
    }
    if (hDelta != 0) {
        EmitEvent(m_mouseFd, EV_REL, REL_HWHEEL_HI_RES, hDelta);
        m_scrollAccumH += hDelta;
        while (m_scrollAccumH >= 120) { EmitEvent(m_mouseFd, EV_REL, REL_HWHEEL, 1);  m_scrollAccumH -= 120; }
        while (m_scrollAccumH <= -120){ EmitEvent(m_mouseFd, EV_REL, REL_HWHEEL, -1); m_scrollAccumH += 120; }
    }
    SynReport(m_mouseFd);
}

void LinuxInputInjector::Button(MouseButtonId button, bool down) {
    if (!EnsureMouse()) return;
    uint16_t code = BTN_LEFT;
    switch (button) {
    case MouseButtonId::Left:   code = BTN_LEFT;   break;
    case MouseButtonId::Right:  code = BTN_RIGHT;  break;
    case MouseButtonId::Middle: code = BTN_MIDDLE; break;
    case MouseButtonId::X1:     code = BTN_SIDE;   break;
    case MouseButtonId::X2:     code = BTN_EXTRA;  break;
    }
    EmitEvent(m_mouseFd, EV_KEY, code, down ? 1 : 0);
    SynReport(m_mouseFd);
}

void LinuxInputInjector::Key(uint16_t keyId, bool down) {
    if (!EnsureKeyboard()) return;
    const uint16_t code = EvdevKeyFromKeyId(keyId);
    if (code == 0) return;
    EmitEvent(m_keyboardFd, EV_KEY, code, down ? 1 : 0);
    SynReport(m_keyboardFd);
}

void LinuxInputInjector::ReleaseAll() {
    // Nothing tracked below ControllerManager's own held-input state; see
    // IInputInjector::ReleaseAll's contract.
}

std::string LinuxInputInjector::KeyDisplayName(uint16_t keyId) const {
    // No XKB layout lookup here (that would need libxkbcommon wired to the
    // compositor's active keymap) — fall back to the physical-position name
    // from the shared catalog, which is at least stable and accurate for a
    // US layout.
    const std::string jsCode = JsCodeFromKeyId(keyId);
    return jsCode.empty() ? std::string("Key") : jsCode;
}

std::chrono::milliseconds LinuxInputInjector::KeyRepeatDelay() const {
    return std::chrono::milliseconds(400);  // a reasonable fixed default; no XKB session to read
}

std::chrono::milliseconds LinuxInputInjector::KeyRepeatInterval() const {
    return std::chrono::milliseconds(40);
}

void LinuxInputInjector::LogEnvironment(const char* reason) {
    EventLog::Write("INJECT: %s — Linux uinput backend, no foreground/UIPI concept to report", reason);
}

void LinuxInputInjector::LogCursorNotMoving(long, long, long) {
    // Wayland gives no way to read the cursor position back, so this
    // diagnostic (a real signal on Windows that a cursor clip or hook is
    // eating injected motion) has nothing to compare against here.
}
