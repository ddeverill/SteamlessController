#include "VirtualController.h"
#include "steam/SteamController.h"
#include <ViGEmClient.h>
#include <cstdio>
#include <algorithm>
#include <mutex>
#include <utility>

static std::mutex g_notificationMutex;
static VirtualController* g_notificationSink = nullptr;

static VOID X360Notification(
    PVIGEM_CLIENT,
    PVIGEM_TARGET,
    UCHAR largeMotor,
    UCHAR smallMotor,
    UCHAR)
{
    std::lock_guard<std::mutex> lock(g_notificationMutex);
    if (g_notificationSink)
        g_notificationSink->OnRumble(largeMotor, smallMotor);
}

static VOID DS4Notification(
    PVIGEM_CLIENT,
    PVIGEM_TARGET,
    UCHAR largeMotor,
    UCHAR smallMotor,
    DS4_LIGHTBAR_COLOR)
{
    std::lock_guard<std::mutex> lock(g_notificationMutex);
    if (g_notificationSink)
        g_notificationSink->OnRumble(largeMotor, smallMotor);
}

// ---------------------------------------------------------------------------
// Report translation — 0x45 → XUSB_REPORT
// ---------------------------------------------------------------------------

static void ApplyBackActionX360(BackButtonAction action, XUSB_REPORT& r) {
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
    case BackButtonAction::Menu:      r.wButtons |= XUSB_GAMEPAD_START;          break;
    case BackButtonAction::View:      r.wButtons |= XUSB_GAMEPAD_BACK;           break;
    case BackButtonAction::L3:        r.wButtons |= XUSB_GAMEPAD_LEFT_THUMB;     break;
    case BackButtonAction::R3:        r.wButtons |= XUSB_GAMEPAD_RIGHT_THUMB;    break;
    case BackButtonAction::None:
    case BackButtonAction::LeftMouseButton:
    case BackButtonAction::RightMouseButton:
    default: break;
    }
}

static XUSB_REPORT TranslateX360(const uint8_t* buf, size_t n) {
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
    if (b1 & SteamController::BTN_DPAD_UP) r.wButtons |= XUSB_GAMEPAD_DPAD_UP;
    if (b1 & SteamController::BTN_DPAD_DN) r.wButtons |= XUSB_GAMEPAD_DPAD_DOWN;
    if (b1 & SteamController::BTN_DPAD_LT) r.wButtons |= XUSB_GAMEPAD_DPAD_LEFT;
    if (b1 & SteamController::BTN_DPAD_RT) r.wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;

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
// Report translation — 0x45 → DS4_REPORT
// ---------------------------------------------------------------------------

// Maps signed 16-bit XInput range to DS4 unsigned byte (0=min, 128=center, 255=max).
static uint8_t s16ToU8(int16_t v) {
    return static_cast<uint8_t>((static_cast<int32_t>(v) + 32768) >> 8);
}

static DS4_REPORT TranslateDS4(const uint8_t* buf, size_t n,
                                const BackButtonConfig& backCfg, bool backMouseEnabled) {
    DS4_REPORT r{};
    // Zero-init gives sticks at 0; we set them from actual data below, and
    // always call DS4_SET_DPAD at the end — so no need for DS4_REPORT_INIT.
    if (n < 18) {
        // Return centered sticks and D-pad none as safe default.
        r.bThumbLX = r.bThumbLY = r.bThumbRX = r.bThumbRY = 0x80;
        DS4_SET_DPAD(&r, DS4_BUTTON_DPAD_NONE);
        return r;
    }

    const uint8_t b0 = buf[2];
    const uint8_t b1 = buf[3];
    const uint8_t b2 = buf[4];

    // Face buttons (A→Cross, B→Circle, X→Square, Y→Triangle)
    if (b0 & SteamController::BTN_A) r.wButtons |= DS4_BUTTON_CROSS;
    if (b0 & SteamController::BTN_B) r.wButtons |= DS4_BUTTON_CIRCLE;
    if (b0 & SteamController::BTN_X) r.wButtons |= DS4_BUTTON_SQUARE;
    if (b0 & SteamController::BTN_Y) r.wButtons |= DS4_BUTTON_TRIANGLE;

    // Bumpers
    if (b2 & SteamController::BTN_LB) r.wButtons |= DS4_BUTTON_SHOULDER_LEFT;
    if (b1 & SteamController::BTN_RB) r.wButtons |= DS4_BUTTON_SHOULDER_RIGHT;

    // Options / Share (Menu→Options, View→Share)
    if (b0 & SteamController::BTN_MENU) r.wButtons |= DS4_BUTTON_OPTIONS;
    if (b1 & SteamController::BTN_VIEW) r.wButtons |= DS4_BUTTON_SHARE;

    // Stick clicks
    if (b1 & SteamController::BTN_LS) r.wButtons |= DS4_BUTTON_THUMB_LEFT;
    if (b0 & SteamController::BTN_RS) r.wButtons |= DS4_BUTTON_THUMB_RIGHT;

    // PS / Steam button
    if (b2 & SteamController::BTN_STEAM) r.bSpecial |= DS4_SPECIAL_BUTTON_PS;

    // Right trackpad click → DS4 touchpad button
    if (b2 & SteamController::BTN_TP_RT) r.bSpecial |= DS4_SPECIAL_BUTTON_TOUCHPAD;

    // D-pad: accumulate all direction sources before computing the HAT.
    bool dUp = (b1 & SteamController::BTN_DPAD_UP) != 0;
    bool dDn = (b1 & SteamController::BTN_DPAD_DN) != 0;
    bool dLt = (b1 & SteamController::BTN_DPAD_LT) != 0;
    bool dRt = (b1 & SteamController::BTN_DPAD_RT) != 0;

    // Triggers: 16-bit signed (0x0000–0x7FFF) → 8-bit (0–255)
    int16_t ltRaw, rtRaw;
    memcpy(&ltRaw, buf + 6, 2);
    memcpy(&rtRaw, buf + 8, 2);
    r.bTriggerL = static_cast<uint8_t>(std::clamp<int>(ltRaw >> 7, 0, 255));
    r.bTriggerR = static_cast<uint8_t>(std::clamp<int>(rtRaw >> 7, 0, 255));

    // Sticks: signed 16-bit → uint8 centered at 0x80.
    // DS4 Y axis is inverted vs XInput (0 = up, 255 = down).
    int16_t lx, ly, rx, ry;
    memcpy(&lx, buf + 10, 2);
    memcpy(&ly, buf + 12, 2);
    memcpy(&rx, buf + 14, 2);
    memcpy(&ry, buf + 16, 2);
    r.bThumbLX = s16ToU8(lx);
    r.bThumbLY = static_cast<uint8_t>(255u - s16ToU8(ly));
    r.bThumbRX = s16ToU8(rx);
    r.bThumbRY = static_cast<uint8_t>(255u - s16ToU8(ry));

    // Back paddles: apply to report and accumulate D-pad bits.
    auto applyBack = [&](BackButtonAction action) {
        switch (action) {
        case BackButtonAction::A:         r.wButtons |= DS4_BUTTON_CROSS;          break;
        case BackButtonAction::B:         r.wButtons |= DS4_BUTTON_CIRCLE;         break;
        case BackButtonAction::X:         r.wButtons |= DS4_BUTTON_SQUARE;         break;
        case BackButtonAction::Y:         r.wButtons |= DS4_BUTTON_TRIANGLE;       break;
        case BackButtonAction::LB:        r.wButtons |= DS4_BUTTON_SHOULDER_LEFT;  break;
        case BackButtonAction::RB:        r.wButtons |= DS4_BUTTON_SHOULDER_RIGHT; break;
        case BackButtonAction::LT:        r.bTriggerL = 255;                       break;
        case BackButtonAction::RT:        r.bTriggerR = 255;                       break;
        case BackButtonAction::DPadUp:    dUp = true;                              break;
        case BackButtonAction::DPadDown:  dDn = true;                              break;
        case BackButtonAction::DPadLeft:  dLt = true;                              break;
        case BackButtonAction::DPadRight: dRt = true;                              break;
        case BackButtonAction::Menu:      r.wButtons |= DS4_BUTTON_OPTIONS;        break;
        case BackButtonAction::View:      r.wButtons |= DS4_BUTTON_SHARE;          break;
        case BackButtonAction::L3:        r.wButtons |= DS4_BUTTON_THUMB_LEFT;     break;
        case BackButtonAction::R3:        r.wButtons |= DS4_BUTTON_THUMB_RIGHT;    break;
        default: break;
        }
    };

    if (n > 4) {
        if (!backMouseEnabled && (buf[4] & SteamController::BTN_L4)) applyBack(backCfg.l4);
        if (buf[4] & SteamController::BTN_L5) applyBack(backCfg.l5);
    }
    if (n > 2) {
        if (!backMouseEnabled && (buf[2] & SteamController::BTN_R4)) applyBack(backCfg.r4);
    }
    if (n > 3) {
        if (buf[3] & SteamController::BTN_R5) applyBack(backCfg.r5);
    }

    // Resolve accumulated D-pad bits into a HAT value (handles diagonals).
    DS4_DPAD_DIRECTIONS hat = DS4_BUTTON_DPAD_NONE;
    if      (dUp && dRt) hat = DS4_BUTTON_DPAD_NORTHEAST;
    else if (dUp && dLt) hat = DS4_BUTTON_DPAD_NORTHWEST;
    else if (dDn && dRt) hat = DS4_BUTTON_DPAD_SOUTHEAST;
    else if (dDn && dLt) hat = DS4_BUTTON_DPAD_SOUTHWEST;
    else if (dUp)        hat = DS4_BUTTON_DPAD_NORTH;
    else if (dRt)        hat = DS4_BUTTON_DPAD_EAST;
    else if (dDn)        hat = DS4_BUTTON_DPAD_SOUTH;
    else if (dLt)        hat = DS4_BUTTON_DPAD_WEST;
    DS4_SET_DPAD(&r, hat);

    return r;
}

// ---------------------------------------------------------------------------
// VirtualController
// ---------------------------------------------------------------------------

VirtualController::VirtualController(ControllerPlatform platform, RumbleCallback rumbleCallback)
    : m_platform(platform), m_rumbleCallback(std::move(rumbleCallback))
{
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

    m_target = (platform == ControllerPlatform::Xbox)
        ? vigem_target_x360_alloc()
        : vigem_target_ds4_alloc();
    if (!m_target) { printf("[ViGEm] target alloc failed\n"); return; }

    err = vigem_target_add(static_cast<PVIGEM_CLIENT>(m_client),
                           static_cast<PVIGEM_TARGET>(m_target));
    if (!VIGEM_SUCCESS(err)) {
        printf("[ViGEm] target_add failed: 0x%08X\n", err);
        vigem_target_free(static_cast<PVIGEM_TARGET>(m_target));
        m_target = nullptr;
        return;
    }

    if (platform == ControllerPlatform::Xbox) {
        printf("[ViGEm] Virtual Xbox 360 controller connected\n");
        err = vigem_target_x360_register_notification(
            static_cast<PVIGEM_CLIENT>(m_client),
            static_cast<PVIGEM_TARGET>(m_target),
            X360Notification);
    } else {
        printf("[ViGEm] Virtual DualShock 4 controller connected\n");
        err = vigem_target_ds4_register_notification(
            static_cast<PVIGEM_CLIENT>(m_client),
            static_cast<PVIGEM_TARGET>(m_target),
            DS4Notification);
    }

    if (!VIGEM_SUCCESS(err))
        printf("[ViGEm] notification registration failed: 0x%08X\n", err);
    else {
        std::lock_guard<std::mutex> lock(g_notificationMutex);
        g_notificationSink = this;
    }

    m_valid = true;
}

VirtualController::~VirtualController() {
    if (m_target) {
        if (m_platform == ControllerPlatform::Xbox)
            vigem_target_x360_unregister_notification(static_cast<PVIGEM_TARGET>(m_target));
        else
            vigem_target_ds4_unregister_notification(static_cast<PVIGEM_TARGET>(m_target));
        std::lock_guard<std::mutex> lock(g_notificationMutex);
        if (g_notificationSink == this)
            g_notificationSink = nullptr;
    }

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

    if (m_platform == ControllerPlatform::Xbox) {
        XUSB_REPORT report = TranslateX360(buf, n);

        if (n > 4) {
            if (!backMouseEnabled && (buf[4] & SteamController::BTN_L4)) ApplyBackActionX360(backCfg.l4, report);
            if (buf[4] & SteamController::BTN_L5) ApplyBackActionX360(backCfg.l5, report);
        }
        if (n > 2) {
            if (!backMouseEnabled && (buf[2] & SteamController::BTN_R4)) ApplyBackActionX360(backCfg.r4, report);
        }
        if (n > 3) {
            if (buf[3] & SteamController::BTN_R5) ApplyBackActionX360(backCfg.r5, report);
        }

        vigem_target_x360_update(static_cast<PVIGEM_CLIENT>(m_client),
                                 static_cast<PVIGEM_TARGET>(m_target),
                                 report);
    } else {
        DS4_REPORT report = TranslateDS4(buf, n, backCfg, backMouseEnabled);
        vigem_target_ds4_update(static_cast<PVIGEM_CLIENT>(m_client),
                                static_cast<PVIGEM_TARGET>(m_target),
                                report);
    }
}

void VirtualController::OnRumble(uint8_t largeMotor, uint8_t smallMotor) {
    if (m_rumbleCallback)
        m_rumbleCallback(largeMotor, smallMotor);
}
