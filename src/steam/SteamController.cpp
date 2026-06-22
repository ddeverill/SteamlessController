#include "SteamController.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Internal helper: build a 64-byte feature report command buffer.
// ---------------------------------------------------------------------------

static void BuildCmd(uint8_t (&buf)[64], uint8_t cmd,
                     const uint8_t* payload = nullptr, uint8_t payloadSize = 0) {
    std::memset(buf, 0, 64);
    buf[0] = SteamController::FEATURE_REPORT_CMD;
    buf[1] = cmd;
    buf[2] = payloadSize;
    if (payload && payloadSize)
        std::memcpy(buf + 3, payload, payloadSize);
}

static void WriteU16LE(uint8_t* p, uint16_t v) {
    std::memcpy(p, &v, sizeof(v));
}

// ---------------------------------------------------------------------------
// Haptic tuning helpers
// ---------------------------------------------------------------------------

static double ClampUnit(double v) { return std::clamp(v, 0.0, 1.0); }

static double MotorByteToUnit(uint8_t value) {
    static constexpr double kDeadzone = 6.0;
    if (value <= kDeadzone) return 0.0;
    return (static_cast<double>(value) - kDeadzone) / (255.0 - kDeadzone);
}

static uint16_t UnitToHapticSpeed(double value, uint16_t minimumSpeed) {
    value = ClampUnit(value);
    if (value <= 0.0) return 0;
    const double curved = std::pow(value, 0.68);
    const double speed  = static_cast<double>(minimumSpeed)
                        + curved * static_cast<double>(0xFFFFu - minimumSpeed);
    return static_cast<uint16_t>(std::clamp<int>(static_cast<int>(std::lround(speed)), 0, 0xFFFF));
}

static constexpr uint8_t  HAPTIC_COMMAND_TICK  = 1;
static constexpr uint8_t  HAPTIC_COMMAND_CLICK = 2;
static constexpr uint16_t TRACKPAD_CLICK_PULSE_US       = 5000;
static constexpr int8_t   TRACKPAD_TOUCH_GAIN_DB        = -45;
static constexpr int8_t   TRACKPAD_CLICK_COMMAND_GAIN_DB = 9;
static constexpr int16_t  TRACKPAD_CLICK_PULSE_GAIN_DB  = 40;

// ---------------------------------------------------------------------------
// Open / Close
// ---------------------------------------------------------------------------

std::vector<std::wstring> SteamController::EnumerateAll() {
    std::vector<std::wstring> result;
    for (uint16_t pid : { SC2026_PID, SC2026_DONGLE_PID }) {
        auto paths = HidDevice::Enumerate(VALVE_VID, pid, VENDOR_USAGE_PAGE);
        for (auto const& path : paths) {
            HidDevice dev;
            if (!dev.Open(path)) continue;
            uint8_t buf[64];
            size_t n = dev.ReadInputReport(buf, sizeof(buf), /*timeoutMs=*/500);
            if (n > 0 && IsStateReportId(buf[0]))
                result.push_back(path);
            else if (n > 0)
                printf("Unexpected report ID 0x%02X on PID=%04X — skipping.\n", buf[0], pid);
        }
    }
    return result;
}

bool SteamController::Open() {
    auto paths = EnumerateAll();
    if (paths.empty()) {
        printf("No Steam Controller found (wired PID=%04X or dongle PID=%04X).\n",
               SC2026_PID, SC2026_DONGLE_PID);
        return false;
    }
    return Open(paths[0]);
}

bool SteamController::Open(const std::wstring& path) {
    if (!m_device.Open(path)) return false;
    printf("[SC] Opened controller at path %ls\n", path.c_str());
    return true;
}

void SteamController::Close() {
    if (m_running.exchange(false) && m_heartbeat.joinable())
        m_heartbeat.join();
    ClearTrackpadHaptics();
    SetRumble(0, 0);
    m_device.Close();
}

// ---------------------------------------------------------------------------
// Lizard mode
// ---------------------------------------------------------------------------

bool SteamController::DisableLizardMode() {
    uint8_t buf[64];

    BuildCmd(buf, CMD_CLEAR_DIGITAL_MAPPINGS);
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        if (!m_device.SendFeatureReport(buf, sizeof(buf))) {
            printf("Failed to send CLEAR_DIGITAL_MAPPINGS.\n");
            return false;
        }
    }

    // Keep IMU disabled until a virtual output mode that needs it explicitly enables it.
    const uint8_t imuOff[] = { SETTING_IMU_MODE, 0x00, 0x00 };
    BuildCmd(buf, CMD_SET_SETTINGS, imuOff, sizeof(imuOff));
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        if (!m_device.SendFeatureReport(buf, sizeof(buf))) {
            printf("Failed to disable IMU mode.\n");
            return false;
        }
    }

    const uint8_t settingsPayload[] = {
        SETTING_LEFT_TRACKPAD_MODE,  0x00, 0x00,
        SETTING_RIGHT_TRACKPAD_MODE, 0x00, 0x00,
    };
    BuildCmd(buf, CMD_SET_SETTINGS, settingsPayload, sizeof(settingsPayload));
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        if (!m_device.SendFeatureReport(buf, sizeof(buf))) {
            printf("Failed to send SET_SETTINGS_VALUES.\n");
            return false;
        }
    }

    if (!m_running.exchange(true))
        m_heartbeat = std::thread(&SteamController::HeartbeatLoop, this);

    return true;
}

bool SteamController::EnableLizardMode() {
    if (m_running.exchange(false) && m_heartbeat.joinable())
        m_heartbeat.join();

    ClearTrackpadHaptics();
    SetRumble(0, 0);
    SetImuEnabled(false);

    uint8_t buf[64];
    BuildCmd(buf, CMD_SET_DEFAULT_MAPPINGS);
    std::lock_guard<std::mutex> lock(m_writeMutex);
    return m_device.SendFeatureReport(buf, sizeof(buf));
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

size_t SteamController::ReadReport(uint8_t* buffer, size_t size, uint32_t timeoutMs) {
    return m_device.ReadInputReport(buffer, size, timeoutMs);
}

// ---------------------------------------------------------------------------
// IMU
// ---------------------------------------------------------------------------

bool SteamController::SetImuEnabled(bool enabled) {
    const uint16_t value = enabled ? IMU_MODE_RAW_ACCEL_GYRO : IMU_MODE_OFF;
    const uint8_t payload[] = {
        SETTING_IMU_MODE,
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
    };
    uint8_t buf[64];
    BuildCmd(buf, CMD_SET_SETTINGS, payload, sizeof(payload));
    std::lock_guard<std::mutex> lock(m_writeMutex);
    if (!m_device.SendFeatureReport(buf, sizeof(buf))) {
        printf("Failed to %s IMU mode.\n", enabled ? "enable" : "disable");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Rumble — enhanced haptic model
//
// Both XInput large/small motor channels map to the same physical layout on
// the Steam Controller as on real Xbox/DS4 hardware: large (low-freq) lives
// in the left grip, small (high-freq) in the right grip.  We cross-mix the
// two channels across both haptic actuators and add a short "attack boost" on
// rising edges to replicate the punch of ERM motors starting from rest.
// ---------------------------------------------------------------------------

void SteamController::SetRumble(uint8_t largeMotor, uint8_t smallMotor) {
    const double low  = MotorByteToUnit(largeMotor);
    const double high = MotorByteToUnit(smallMotor);

    const double leftDemand  = ClampUnit(low * 1.00 + high * 0.30);
    const double rightDemand = ClampUnit(low * 0.78 + high * 0.88);
    const uint16_t baseLeft  = UnitToHapticSpeed(leftDemand,  0x1200);
    const uint16_t baseRight = UnitToHapticSpeed(rightDemand, 0x1200);

    const auto now = std::chrono::steady_clock::now();
    RumbleFrame frame{};
    {
        std::lock_guard<std::mutex> lock(m_rumbleMutex);

        const int largeRise = m_hasRumbleState
            ? static_cast<int>(largeMotor) - static_cast<int>(m_lastLargeMotor) : largeMotor;
        const int smallRise = m_hasRumbleState
            ? static_cast<int>(smallMotor) - static_cast<int>(m_lastSmallMotor) : smallMotor;

        m_lastLargeMotor  = largeMotor;
        m_lastSmallMotor  = smallMotor;
        m_hasRumbleState  = true;
        m_rumbleBaseLeft  = baseLeft;
        m_rumbleBaseRight = baseRight;

        if (baseLeft == 0 && baseRight == 0) {
            m_rumbleBoostLeft  = 0;
            m_rumbleBoostRight = 0;
            m_rumbleBoostUntil = {};
        } else {
            const double largePunch = (largeRise > 0 ? largeRise : 0) / 255.0 * 0.55;
            const double smallPunch = (smallRise > 0 ? smallRise : 0) / 255.0 * 0.38;
            const double punch = largePunch > smallPunch ? largePunch : smallPunch;

            if (punch >= 0.08) {
                const double boostL = ClampUnit(leftDemand  + punch * 0.70 + low  * 0.08);
                const double boostR = ClampUnit(rightDemand + punch * 0.62 + high * 0.10);
                m_rumbleBoostLeft  = UnitToHapticSpeed(boostL, 0x2200);
                m_rumbleBoostRight = UnitToHapticSpeed(boostR, 0x2200);
                m_rumbleBoostUntil = now + std::chrono::milliseconds(55 + (largeMotor >= 180 ? 25 : 0));
            }
        }

        m_lastRumbleSent = now;
        frame = CurrentRumbleFrameLocked(now);
    }

    SendRumbleOutput(frame.left, frame.right);
}

SteamController::RumbleFrame SteamController::CurrentRumbleFrameLocked(
    std::chrono::steady_clock::time_point now) const
{
    RumbleFrame frame{m_rumbleBaseLeft, m_rumbleBaseRight};
    if (m_rumbleBoostUntil > now) {
        if (m_rumbleBoostLeft  > frame.left)  frame.left  = m_rumbleBoostLeft;
        if (m_rumbleBoostRight > frame.right) frame.right = m_rumbleBoostRight;
    }
    return frame;
}

// ---------------------------------------------------------------------------
// Trackpad haptics
// ---------------------------------------------------------------------------

void SteamController::PulseTrackpadHaptic(bool left, bool strongClick) {
    const uint8_t side    = left ? 0x01 : 0x02;
    const uint8_t command = strongClick ? HAPTIC_COMMAND_CLICK : HAPTIC_COMMAND_TICK;
    const int8_t  gainDb  = strongClick ? TRACKPAD_CLICK_COMMAND_GAIN_DB : TRACKPAD_TOUCH_GAIN_DB;
    SendTrackpadCommandOutput(side, command, gainDb);
    if (strongClick)
        SendTrackpadPulseOutput(side, TRACKPAD_CLICK_PULSE_US, 0, 1, TRACKPAD_CLICK_PULSE_GAIN_DB);
}

void SteamController::ClearTrackpadHaptics() {
    // Pulse/command reports are one-shots — no persistent state to cancel.
    // Kept as a lifecycle hook for clean teardown.
}

// ---------------------------------------------------------------------------
// Low-level output
// ---------------------------------------------------------------------------

bool SteamController::SendRumbleOutput(uint16_t leftSpeed, uint16_t rightSpeed) {
    if (!m_device.IsOpen()) return false;

    uint8_t buf[10] = {};
    buf[0] = OUT_HAPTIC_RUMBLE;
    buf[1] = 0x00;
    WriteU16LE(buf + 2, 0x0000);   // intensity (unused field)
    WriteU16LE(buf + 4, leftSpeed);
    buf[6] = 0x00;                 // left gain
    WriteU16LE(buf + 7, rightSpeed);
    buf[9] = 0x00;                 // right gain

    std::lock_guard<std::mutex> lock(m_writeMutex);
    return m_device.WriteOutputReport(buf, sizeof(buf));
}

bool SteamController::SendTrackpadPulseOutput(uint8_t side, uint16_t onUs, uint16_t offUs,
                                               uint16_t repeatCount, int16_t gainDb) {
    if (!m_device.IsOpen()) return false;

    uint8_t buf[10] = {};
    buf[0] = OUT_HAPTIC_PULSE;
    buf[1] = side;
    WriteU16LE(buf + 2, onUs);
    WriteU16LE(buf + 4, offUs);
    WriteU16LE(buf + 6, repeatCount);
    WriteU16LE(buf + 8, static_cast<uint16_t>(gainDb));

    std::lock_guard<std::mutex> lock(m_writeMutex);
    return m_device.WriteOutputReport(buf, sizeof(buf));
}

bool SteamController::SendTrackpadCommandOutput(uint8_t side, uint8_t command, int8_t gainDb) {
    if (!m_device.IsOpen()) return false;

    uint8_t buf[4] = {};
    buf[0] = OUT_HAPTIC_COMMAND;
    buf[1] = side;
    buf[2] = command;
    buf[3] = static_cast<uint8_t>(gainDb);

    std::lock_guard<std::mutex> lock(m_writeMutex);
    return m_device.WriteOutputReport(buf, sizeof(buf));
}

// ---------------------------------------------------------------------------
// Heartbeat — keeps lizard mode suppressed and re-sends active rumble
// ---------------------------------------------------------------------------

void SteamController::HeartbeatLoop() {
    uint8_t buf[64];
    BuildCmd(buf, CMD_CLEAR_DIGITAL_MAPPINGS);
    int lizardTicks = 0;

    while (m_running.load()) {
        if (lizardTicks <= 0) {
            std::lock_guard<std::mutex> lock(m_writeMutex);
            m_device.SendFeatureReport(buf, sizeof(buf));
            lizardTicks = 20;
        }

        RumbleFrame frame{};
        {
            std::lock_guard<std::mutex> lock(m_rumbleMutex);
            frame = CurrentRumbleFrameLocked(std::chrono::steady_clock::now());
        }
        if (frame.left || frame.right)
            SendRumbleOutput(frame.left, frame.right);

        --lizardTicks;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
}
