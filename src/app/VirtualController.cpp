#include "VirtualController.h"
#include "steam/SteamController.h"
#include <ViGEm/Client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>

static_assert(sizeof(DS4_REPORT_EX) == 63, "DS4_REPORT_EX must be 63 bytes");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static VOID CALLBACK X360Notification(
    PVIGEM_CLIENT,
    PVIGEM_TARGET,
    UCHAR largeMotor,
    UCHAR smallMotor,
    UCHAR /*ledNumber*/,
    LPVOID userData)
{
    if (userData)
        static_cast<VirtualController*>(userData)->OnRumble(largeMotor, smallMotor);
}

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

// ---------------------------------------------------------------------------
// Report translation — STATE → XUSB_REPORT
// ---------------------------------------------------------------------------

static XUSB_REPORT TranslateX360(const uint8_t* buf, size_t n) {
    XUSB_REPORT r{};
    if (n < 18) return r;

    const uint8_t b0 = buf[2];
    const uint8_t b1 = buf[3];
    const uint8_t b2 = buf[4];

    if (b0 & SteamController::BTN_A) r.wButtons |= XUSB_GAMEPAD_A;
    if (b0 & SteamController::BTN_B) r.wButtons |= XUSB_GAMEPAD_B;
    if (b0 & SteamController::BTN_X) r.wButtons |= XUSB_GAMEPAD_X;
    if (b0 & SteamController::BTN_Y) r.wButtons |= XUSB_GAMEPAD_Y;

    if (b2 & SteamController::BTN_LB) r.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if (b1 & SteamController::BTN_RB) r.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;

    if (b0 & SteamController::BTN_MENU) r.wButtons |= XUSB_GAMEPAD_START;
    if (b1 & SteamController::BTN_VIEW) r.wButtons |= XUSB_GAMEPAD_BACK;

    if (b1 & SteamController::BTN_LS) r.wButtons |= XUSB_GAMEPAD_LEFT_THUMB;
    if (b0 & SteamController::BTN_RS) r.wButtons |= XUSB_GAMEPAD_RIGHT_THUMB;

    if (b2 & SteamController::BTN_STEAM) r.wButtons |= XUSB_GAMEPAD_GUIDE;

    if (b1 & SteamController::BTN_DPAD_UP) r.wButtons |= XUSB_GAMEPAD_DPAD_UP;
    if (b1 & SteamController::BTN_DPAD_DN) r.wButtons |= XUSB_GAMEPAD_DPAD_DOWN;
    if (b1 & SteamController::BTN_DPAD_LT) r.wButtons |= XUSB_GAMEPAD_DPAD_LEFT;
    if (b1 & SteamController::BTN_DPAD_RT) r.wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;

    int16_t ltRaw, rtRaw;
    memcpy(&ltRaw, buf + 6, 2);
    memcpy(&rtRaw, buf + 8, 2);
    r.bLeftTrigger  = static_cast<uint8_t>(std::clamp<int>(ltRaw >> 7, 0, 255));
    r.bRightTrigger = static_cast<uint8_t>(std::clamp<int>(rtRaw >> 7, 0, 255));

    memcpy(&r.sThumbLX, buf + 10, 2);
    memcpy(&r.sThumbLY, buf + 12, 2);
    memcpy(&r.sThumbRX, buf + 14, 2);
    memcpy(&r.sThumbRY, buf + 16, 2);

    return r;
}

// ---------------------------------------------------------------------------
// Report translation — STATE → DS4_REPORT_EX (with gyro, touchpad, battery)
// ---------------------------------------------------------------------------

static uint8_t s16ToU8(int16_t v) {
    return static_cast<uint8_t>((static_cast<int32_t>(v) + 32768) >> 8);
}

static int16_t NegI16(int16_t v) {
    return static_cast<int16_t>((v == -32768) ? 32767 : -v);
}

// Pack two 12-bit XY values into the 3-byte DS4 touch data encoding.
static void PackDs4Touch(uint8_t data[3], int x, int y) {
    data[0] = static_cast<uint8_t>(x & 0xFF);
    data[1] = static_cast<uint8_t>(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
    data[2] = static_cast<uint8_t>((y >> 4) & 0xFF);
}

// Map a signed 16-bit SC pad axis to a DS4 touch coordinate (0..maxValue).
static int NormalizePadAxis(int16_t raw, int maxValue, bool invert) {
    int v = raw;
    if (invert) v = (v == -32768) ? 32767 : -v;
    const int normalized = static_cast<int>(
        (static_cast<int64_t>(v) + 32768) * maxValue / 65535);
    return std::clamp(normalized, 0, maxValue);
}

// Extract large/small motor from a DS4 output report buffer.
static bool ParseDs4OutputRumble(const DS4_OUTPUT_BUFFER& output,
                                  uint8_t& largeMotor, uint8_t& smallMotor) {
    const uint8_t* b = output.Buffer;
    if (b[0] == 0x05) { smallMotor = b[3]; largeMotor = b[4]; return true; }
    if (b[0] == 0x11) { smallMotor = b[6]; largeMotor = b[7]; return true; }
    return false;
}

// Set the D-pad HAT bits (lower 4 bits of wButtons) on the EX report.
// DS4_SET_DPAD macro is not usable here because it expects PDS4_REPORT (the simple struct).
static void SetDs4DPad(USHORT& wButtons, DS4_DPAD_DIRECTIONS dir) {
    wButtons = (wButtons & ~static_cast<USHORT>(0xF)) | static_cast<USHORT>(dir);
}

// ---------------------------------------------------------------------------
// VirtualController
// ---------------------------------------------------------------------------

VirtualController::VirtualController(ControllerPlatform platform, RumbleCallback rumbleCallback)
    : m_platform(platform), m_rumbleCallback(std::move(rumbleCallback))
{
    m_client = vigem_alloc();
    if (!m_client) {
        m_failStage = "vigem_alloc";
        printf("[ViGEm] alloc failed\n");
        return;
    }

    VIGEM_ERROR err = vigem_connect(static_cast<PVIGEM_CLIENT>(m_client));
    if (!VIGEM_SUCCESS(err)) {
        m_failStage = "vigem_connect";
        m_lastError = static_cast<uint32_t>(err);
        if (err == VIGEM_ERROR_BUS_NOT_FOUND || err == VIGEM_ERROR_BUS_ACCESS_FAILED)
            m_driverMissing = true;
        vigem_free(static_cast<PVIGEM_CLIENT>(m_client));
        m_client = nullptr;
        return;
    }

    m_target = (platform == ControllerPlatform::PlayStation)
        ? vigem_target_ds4_alloc()
        : vigem_target_x360_alloc();
    if (!m_target) {
        m_failStage = "vigem_target_alloc";
        printf("[ViGEm] target alloc failed\n");
        return;
    }

    err = vigem_target_add(static_cast<PVIGEM_CLIENT>(m_client),
                           static_cast<PVIGEM_TARGET>(m_target));
    if (!VIGEM_SUCCESS(err)) {
        m_failStage = "vigem_target_add";
        m_lastError = static_cast<uint32_t>(err);
        printf("[ViGEm] target_add failed: 0x%08X\n", err);
        vigem_target_free(static_cast<PVIGEM_TARGET>(m_target));
        m_target = nullptr;
        return;
    }

    if (platform == ControllerPlatform::PlayStation) {
        printf("[ViGEm] Virtual DualShock 4 controller connected\n");
        StartDs4OutputThread();
    } else {
        printf("[ViGEm] Virtual Xbox 360 controller connected\n");
        err = vigem_target_x360_register_notification(
            static_cast<PVIGEM_CLIENT>(m_client),
            static_cast<PVIGEM_TARGET>(m_target),
            X360Notification,
            this);
        if (!VIGEM_SUCCESS(err)) {
            m_failStage = "vigem_target_x360_register_notification";
            m_lastError = static_cast<uint32_t>(err);
            printf("[ViGEm] notification registration failed: 0x%08X\n", err);
            vigem_target_remove(static_cast<PVIGEM_CLIENT>(m_client),
                                static_cast<PVIGEM_TARGET>(m_target));
            vigem_target_free(static_cast<PVIGEM_TARGET>(m_target));
            m_target = nullptr;
            return;
        }
    }

    m_valid = true;
}

VirtualController::~VirtualController() {
    StopDs4OutputThread();

    if (m_target && m_platform == ControllerPlatform::Xbox)
        vigem_target_x360_unregister_notification(static_cast<PVIGEM_TARGET>(m_target));

    if (m_client && m_target)
        vigem_target_remove(static_cast<PVIGEM_CLIENT>(m_client),
                            static_cast<PVIGEM_TARGET>(m_target));
    if (m_target) vigem_target_free(static_cast<PVIGEM_TARGET>(m_target));
    if (m_client) {
        vigem_disconnect(static_cast<PVIGEM_CLIENT>(m_client));
        vigem_free(static_cast<PVIGEM_CLIENT>(m_client));
    }
}

void VirtualController::StartDs4OutputThread() {
    if (m_ds4OutputRunning.exchange(true)) return;
    m_ds4OutputThread = std::thread(&VirtualController::Ds4OutputLoop, this);
}

void VirtualController::StopDs4OutputThread() {
    m_ds4OutputRunning = false;
    if (m_ds4OutputThread.joinable())
        m_ds4OutputThread.join();
}

void VirtualController::Ds4OutputLoop() {
    while (m_ds4OutputRunning && m_client && m_target) {
        DS4_OUTPUT_BUFFER output{};
        VIGEM_ERROR err = vigem_target_ds4_await_output_report_timeout(
            static_cast<PVIGEM_CLIENT>(m_client),
            static_cast<PVIGEM_TARGET>(m_target),
            100,
            &output);

        if (!m_ds4OutputRunning) break;
        if (err == VIGEM_ERROR_TIMED_OUT) continue;
        if (!VIGEM_SUCCESS(err)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        uint8_t largeMotor = 0, smallMotor = 0;
        if (ParseDs4OutputRumble(output, largeMotor, smallMotor))
            OnRumble(largeMotor, smallMotor);
    }
}

void VirtualController::SetBatteryState(uint8_t levelPercent, uint8_t chargeState) {
    if (chargeState == SteamController::CHARGE_STATE_CHARGING_DONE || levelPercent >= 95) {
        m_ds4BatteryLevel   = 0x0B;
        m_ds4BatterySpecial = 0x0B;
        return;
    }
    const int level = std::clamp((static_cast<int>(levelPercent) * 10 + 50) / 100, 0, 10);
    m_ds4BatteryLevel   = static_cast<uint8_t>(level);
    m_ds4BatterySpecial = static_cast<uint8_t>(0x10 | level);
}

void VirtualController::SetTrackpadMouseClaim(bool enabled, bool useLeftTrackpad) {
    m_trackpadMouseEnabled    = enabled;
    m_useLeftTrackpadForMouse = useLeftTrackpad;
}

void VirtualController::Update(const uint8_t* buf, size_t n,
                               const BackButtonConfig& backCfg, bool backMouseEnabled) {
    if (!m_valid) return;

    if (m_platform == ControllerPlatform::PlayStation) {
        // Suppress a pad from the DS4 touchpad report when it is claimed for mouse use.
        const bool suppressRight = m_trackpadMouseEnabled && !m_useLeftTrackpadForMouse;
        const bool suppressLeft  = m_trackpadMouseEnabled &&  m_useLeftTrackpadForMouse;

        DS4_REPORT_EX ex{};
        auto& r = ex.Report;

        if (n < 18) {
            r.bThumbLX = r.bThumbLY = r.bThumbRX = r.bThumbRY = 0x80;
            SetDs4DPad(r.wButtons, DS4_BUTTON_DPAD_NONE);
        } else {
            const uint8_t b0 = buf[2];
            const uint8_t b1 = buf[3];
            const uint8_t b2 = buf[4];
            const uint8_t b3 = n > 5 ? buf[5] : 0;

            if (b0 & SteamController::BTN_A) r.wButtons |= DS4_BUTTON_CROSS;
            if (b0 & SteamController::BTN_B) r.wButtons |= DS4_BUTTON_CIRCLE;
            if (b0 & SteamController::BTN_X) r.wButtons |= DS4_BUTTON_SQUARE;
            if (b0 & SteamController::BTN_Y) r.wButtons |= DS4_BUTTON_TRIANGLE;
            if (b2 & SteamController::BTN_LB) r.wButtons |= DS4_BUTTON_SHOULDER_LEFT;
            if (b1 & SteamController::BTN_RB) r.wButtons |= DS4_BUTTON_SHOULDER_RIGHT;
            if (b0 & SteamController::BTN_MENU) r.wButtons |= DS4_BUTTON_OPTIONS;
            if (b1 & SteamController::BTN_VIEW) r.wButtons |= DS4_BUTTON_SHARE;
            if (b1 & SteamController::BTN_LS) r.wButtons |= DS4_BUTTON_THUMB_LEFT;
            if (b0 & SteamController::BTN_RS) r.wButtons |= DS4_BUTTON_THUMB_RIGHT;
            if (b2 & SteamController::BTN_STEAM) r.bSpecial |= DS4_SPECIAL_BUTTON_PS;

            bool dUp = (b1 & SteamController::BTN_DPAD_UP) != 0;
            bool dDn = (b1 & SteamController::BTN_DPAD_DN) != 0;
            bool dLt = (b1 & SteamController::BTN_DPAD_LT) != 0;
            bool dRt = (b1 & SteamController::BTN_DPAD_RT) != 0;

            int16_t ltRaw, rtRaw;
            memcpy(&ltRaw, buf + 6, 2);
            memcpy(&rtRaw, buf + 8, 2);
            r.bTriggerL = static_cast<uint8_t>(std::clamp<int>(ltRaw >> 7, 0, 255));
            r.bTriggerR = static_cast<uint8_t>(std::clamp<int>(rtRaw >> 7, 0, 255));
            if (r.bTriggerL > 0) r.wButtons |= DS4_BUTTON_TRIGGER_LEFT;
            if (r.bTriggerR > 0) r.wButtons |= DS4_BUTTON_TRIGGER_RIGHT;

            int16_t lx, ly, rx, ry;
            memcpy(&lx, buf + 10, 2);
            memcpy(&ly, buf + 12, 2);
            memcpy(&rx, buf + 14, 2);
            memcpy(&ry, buf + 16, 2);
            r.bThumbLX = s16ToU8(lx);
            r.bThumbLY = static_cast<uint8_t>(255u - s16ToU8(ly));
            r.bThumbRX = s16ToU8(rx);
            r.bThumbRY = static_cast<uint8_t>(255u - s16ToU8(ry));

            // Back paddles
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

            DS4_DPAD_DIRECTIONS hat = DS4_BUTTON_DPAD_NONE;
            if      (dUp && dRt) hat = DS4_BUTTON_DPAD_NORTHEAST;
            else if (dUp && dLt) hat = DS4_BUTTON_DPAD_NORTHWEST;
            else if (dDn && dRt) hat = DS4_BUTTON_DPAD_SOUTHEAST;
            else if (dDn && dLt) hat = DS4_BUTTON_DPAD_SOUTHWEST;
            else if (dUp)        hat = DS4_BUTTON_DPAD_NORTH;
            else if (dRt)        hat = DS4_BUTTON_DPAD_EAST;
            else if (dDn)        hat = DS4_BUTTON_DPAD_SOUTH;
            else if (dLt)        hat = DS4_BUTTON_DPAD_WEST;
            SetDs4DPad(r.wButtons, hat);

            // Touchpad — right SC pad → DS4 contact 1, left SC pad → DS4 contact 2
            const bool rawRightTouching = (b2 & SteamController::BTN_TP_RT)       != 0;
            const bool rawLeftTouching  = (b3 & SteamController::BTN_TP_LT)       != 0;
            const bool rawRightClick    = (b2 & SteamController::BTN_TP_RT_CLICK) != 0;
            const bool rawLeftClick     = (b3 & SteamController::BTN_TP_LT_CLICK) != 0;

            const bool rightTouching = rawRightTouching && !suppressRight;
            const bool leftTouching  = rawLeftTouching  && !suppressLeft;

            if ((rawRightClick && !suppressRight) || (rawLeftClick && !suppressLeft))
                r.bSpecial |= DS4_SPECIAL_BUTTON_TOUCHPAD;

            if (rightTouching && !m_wasRightTouching)
                m_rightTracking = static_cast<uint8_t>((m_rightTracking + 1) & 0x7F);
            if (leftTouching && !m_wasLeftTouching)
                m_leftTracking  = static_cast<uint8_t>((m_leftTracking  + 1) & 0x7F);
            m_wasRightTouching = rightTouching;
            m_wasLeftTouching  = leftTouching;

            int16_t rPadX = 0, rPadY = 0, lPadX = 0, lPadY = 0;
            if (n >= 28) {
                memcpy(&lPadX, buf + 18, 2);
                memcpy(&lPadY, buf + 20, 2);
                memcpy(&rPadX, buf + 24, 2);
                memcpy(&rPadY, buf + 26, 2);
            }

            DS4_TOUCH& touch = r.sCurrentTouch;
            touch.bPacketCounter    = ++m_touchPacketCounter;
            touch.bIsUpTrackingNum1 = static_cast<uint8_t>(m_rightTracking | (rightTouching ? 0x00 : 0x80));
            touch.bIsUpTrackingNum2 = static_cast<uint8_t>(m_leftTracking  | (leftTouching  ? 0x00 : 0x80));
            PackDs4Touch(touch.bTouchData1,
                         rightTouching ? NormalizePadAxis(rPadX, 1919, false) : 0,
                         rightTouching ? NormalizePadAxis(rPadY, 942,  true)  : 0);
            PackDs4Touch(touch.bTouchData2,
                         leftTouching  ? NormalizePadAxis(lPadX, 1919, false) : 0,
                         leftTouching  ? NormalizePadAxis(lPadY, 942,  true)  : 0);
            r.bTouchPacketsN    = 1;
            r.sPreviousTouch[0] = touch;
            r.sPreviousTouch[1] = touch;

            // IMU — only present when SETTING_IMU_MODE = IMU_MODE_RAW_ACCEL_GYRO (report ≥ 46 bytes)
            if (n >= 46) {
                uint32_t imuTs = 0;
                int16_t accelX = 0, accelY = 0, accelZ = 0;
                int16_t gyroX  = 0, gyroY  = 0, gyroZ  = 0;
                memcpy(&imuTs,  buf + 30, 4);
                memcpy(&accelX, buf + 34, 2);
                memcpy(&accelY, buf + 36, 2);
                memcpy(&accelZ, buf + 38, 2);
                memcpy(&gyroX,  buf + 40, 2);
                memcpy(&gyroY,  buf + 42, 2);
                memcpy(&gyroZ,  buf + 44, 2);

                if (!m_hasLastImuTimestamp || imuTs != m_lastImuTimestamp) {
                    const uint32_t delta = m_hasLastImuTimestamp ? (imuTs - m_lastImuTimestamp) : 0;
                    m_ds4Timestamp = static_cast<uint16_t>(
                        m_ds4Timestamp + static_cast<uint16_t>(std::max<uint32_t>(1, delta / 16)));
                    m_lastImuTimestamp    = imuTs;
                    m_hasLastImuTimestamp = true;
                }

                // SC → DS4 axis remapping: SC Y↔Z swapped, Y negated.
                r.wGyroX  = gyroX;
                r.wGyroY  = gyroZ;
                r.wGyroZ  = NegI16(gyroY);
                r.wAccelX = accelX;
                r.wAccelY = accelZ;
                r.wAccelZ = NegI16(accelY);
            } else {
                ++m_ds4Timestamp;
            }
            r.wTimestamp = m_ds4Timestamp;

            r.bBatteryLvl        = m_ds4BatteryLevel;
            r.bBatteryLvlSpecial = m_ds4BatterySpecial;
        }

        VIGEM_ERROR exErr = vigem_target_ds4_update_ex(
            static_cast<PVIGEM_CLIENT>(m_client),
            static_cast<PVIGEM_TARGET>(m_target),
            ex);
        if (!VIGEM_SUCCESS(exErr))
            printf("[ViGEm] ds4_update_ex failed: 0x%08X (bus driver may need update)\n", exErr);
    } else {
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
    }
}

void VirtualController::OnRumble(uint8_t largeMotor, uint8_t smallMotor) {
    if (m_rumbleCallback)
        m_rumbleCallback(largeMotor, smallMotor);
}
