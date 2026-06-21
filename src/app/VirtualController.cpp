#include "VirtualController.h"
#include "steam/SteamController.h"
#include <ViGEmClient.h>
#include <cstdio>
#include <algorithm>

// ---------------------------------------------------------------------------
// Report translation — 0x45 → XUSB_REPORT
// ---------------------------------------------------------------------------

static void ApplyBackAction(BackButtonAction action, XUSB_REPORT& r) {
    switch (action) {
    case BackButtonAction::A:         r.wButtons |= XUSB_GAMEPAD_A;              break;
    case BackButtonAction::B:         r.wButtons |= XUSB_GAMEPAD_B;              break;
    case BackButtonAction::X:         r.wButtons |= XUSB_GAMEPAD_X;              break;
    case BackButtonAction::Y:         r.wButtons |= XUSB_GAMEPAD_Y;              break;
    case BackButtonAction::LB:        r.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;  break;
    case BackButtonAction::RB:        r.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER; break;
    case BackButtonAction::LT:        r.bLeftTrigger  = 255;                     break;
    case BackButtonAction::RT:        r.bRightTrigger = 255;                     break;
    case BackButtonAction::DPadUp:    r.wButtons |= XUSB_GAMEPAD_DPAD_UP;        break;
    case BackButtonAction::DPadDown:  r.wButtons |= XUSB_GAMEPAD_DPAD_DOWN;      break;
    case BackButtonAction::DPadLeft:  r.wButtons |= XUSB_GAMEPAD_DPAD_LEFT;      break;
    case BackButtonAction::DPadRight: r.wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;     break;
    case BackButtonAction::Menu:       r.wButtons |= XUSB_GAMEPAD_START;        break;
    case BackButtonAction::View:       r.wButtons |= XUSB_GAMEPAD_BACK;         break;
    case BackButtonAction::L3:         r.wButtons |= XUSB_GAMEPAD_LEFT_THUMB;   break;
    case BackButtonAction::R3:         r.wButtons |= XUSB_GAMEPAD_RIGHT_THUMB;  break;
    // Mouse button actions are handled via SendInput in the ReadLoop (edge-detected).
    // None/mouse actions produce no XInput output here.
    case BackButtonAction::None:
    case BackButtonAction::LeftMouseButton:
    case BackButtonAction::RightMouseButton:
    default: break;
    }
}

static XUSB_REPORT Translate(const uint8_t* buf, size_t n) {
    XUSB_REPORT r{};
    if (n < 18) return r;

    const uint8_t b0 = buf[2];
    const uint8_t b1 = buf[3];
    const uint8_t b2 = buf[4];

    // Face buttons
    if (b0 & SteamController::BTN_A) r.wButtons |= XUSB_GAMEPAD_A;
    if (b0 & SteamController::BTN_B) r.wButtons |= XUSB_GAMEPAD_B;
    if (b0 & SteamController::BTN_X) r.wButtons |= XUSB_GAMEPAD_X;
    if (b0 & SteamController::BTN_Y) r.wButtons |= XUSB_GAMEPAD_Y;

    // Bumpers
    if (b2 & SteamController::BTN_LB) r.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if (b1 & SteamController::BTN_RB) r.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;

    // Menu / View (Start / Back)
    if (b0 & SteamController::BTN_MENU) r.wButtons |= XUSB_GAMEPAD_START;
    if (b1 & SteamController::BTN_VIEW) r.wButtons |= XUSB_GAMEPAD_BACK;

    // Stick clicks
    if (b1 & SteamController::BTN_LS) r.wButtons |= XUSB_GAMEPAD_LEFT_THUMB;
    if (b0 & SteamController::BTN_RS) r.wButtons |= XUSB_GAMEPAD_RIGHT_THUMB;

    // Steam / Guide button
    if (b2 & SteamController::BTN_STEAM) r.wButtons |= XUSB_GAMEPAD_GUIDE;

    // D-pad
    if (b1 & SteamController::BTN_DPAD_UP)  r.wButtons |= XUSB_GAMEPAD_DPAD_UP;
    if (b1 & SteamController::BTN_DPAD_DN)  r.wButtons |= XUSB_GAMEPAD_DPAD_DOWN;
    if (b1 & SteamController::BTN_DPAD_LT)  r.wButtons |= XUSB_GAMEPAD_DPAD_LEFT;
    if (b1 & SteamController::BTN_DPAD_RT)  r.wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;

    // Triggers: 16-bit signed (0x0000–0x7FFF) → 8-bit (0–255)
    int16_t ltRaw, rtRaw;
    memcpy(&ltRaw, buf + 6, 2);
    memcpy(&rtRaw, buf + 8, 2);
    r.bLeftTrigger  = static_cast<uint8_t>(std::clamp<int>(ltRaw >> 7, 0, 255));
    r.bRightTrigger = static_cast<uint8_t>(std::clamp<int>(rtRaw >> 7, 0, 255));

    // Sticks: 16-bit signed, same range as XInput — pass through directly
    memcpy(&r.sThumbLX, buf + 10, 2);
    memcpy(&r.sThumbLY, buf + 12, 2);
    memcpy(&r.sThumbRX, buf + 14, 2);
    memcpy(&r.sThumbRY, buf + 16, 2);

    return r;
}

// ---------------------------------------------------------------------------
// VirtualController
// ---------------------------------------------------------------------------

VirtualController::VirtualController() {
    m_client = vigem_alloc();
    if (!m_client) { printf("[ViGEm] alloc failed\n"); return; }

    VIGEM_ERROR err = vigem_connect(static_cast<PVIGEM_CLIENT>(m_client));
    if (!VIGEM_SUCCESS(err)) {
        if (err == VIGEM_ERROR_BUS_NOT_FOUND || err == VIGEM_ERROR_BUS_ACCESS_FAILED)
            m_driverMissing = true;
        vigem_free(static_cast<PVIGEM_CLIENT>(m_client));
        m_client = nullptr;
        return;
    }

    m_target = vigem_target_x360_alloc();
    if (!m_target) { printf("[ViGEm] target alloc failed\n"); return; }

    err = vigem_target_add(static_cast<PVIGEM_CLIENT>(m_client),
                           static_cast<PVIGEM_TARGET>(m_target));
    if (!VIGEM_SUCCESS(err)) {
        printf("[ViGEm] target_add failed: 0x%08X\n", err);
        vigem_target_free(static_cast<PVIGEM_TARGET>(m_target));
        m_target = nullptr;
        return;
    }

    printf("[ViGEm] Virtual Xbox 360 controller connected\n");
    m_valid = true;
}

VirtualController::~VirtualController() {
    if (m_client && m_target) {
        vigem_target_remove(static_cast<PVIGEM_CLIENT>(m_client),
                            static_cast<PVIGEM_TARGET>(m_target));
    }
    if (m_target) vigem_target_free(static_cast<PVIGEM_TARGET>(m_target));
    if (m_client) {
        vigem_disconnect(static_cast<PVIGEM_CLIENT>(m_client));
        vigem_free(static_cast<PVIGEM_CLIENT>(m_client));
    }
}

void VirtualController::Update(const uint8_t* buf, size_t n, const BackButtonConfig& backCfg, bool backMouseEnabled) {
    if (!m_valid) return;
    XUSB_REPORT report = Translate(buf, n);

    // Inject back paddle buttons as their mapped XInput actions.
    // L4 and R4 are skipped when "back buttons as mouse click" is active —
    // TrackpadMouse handles them as left-mouse-button presses instead.
    if (n > 4) {
        if (!backMouseEnabled && (buf[4] & SteamController::BTN_L4)) ApplyBackAction(backCfg.l4, report);
        if (buf[4] & SteamController::BTN_L5) ApplyBackAction(backCfg.l5, report);
    }
    if (n > 2) {
        if (!backMouseEnabled && (buf[2] & SteamController::BTN_R4)) ApplyBackAction(backCfg.r4, report);
    }
    if (n > 3) {
        if (buf[3] & SteamController::BTN_R5) ApplyBackAction(backCfg.r5, report);
    }

    vigem_target_x360_update(static_cast<PVIGEM_CLIENT>(m_client),
                             static_cast<PVIGEM_TARGET>(m_target),
                             report);
}
