#pragma once
#include "hid/HidDevice.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

class SteamController {
public:
    static constexpr uint16_t VALVE_VID        = 0x28DE;
    static constexpr uint16_t SC2026_PID       = 0x1302;  // wired USB
    static constexpr uint16_t SC2026_DONGLE_PID = 0x1304; // wireless dongle ("Steam Controller Puck")

    // HID Usage Page for the vendor collection that carries all game input.
    static constexpr uint16_t VENDOR_USAGE_PAGE = 0xFF00;

    // Input report IDs (device → host)
    static constexpr uint8_t REPORT_STATE         = 0x45;  // BLE/no-quaternion state report
    static constexpr uint8_t REPORT_STATE_LEGACY  = 0x42;  // USB/full state report (same layout)
    static constexpr uint8_t REPORT_BATTERY_STATUS = 0x43; // TritonBatteryStatus_t
    static constexpr uint8_t REPORT_STATUS         = 0x44; //  5 bytes: battery / connection
    static constexpr uint8_t REPORT_UNKNOWN_7B     = 0x7B; // 12 bytes: TBD
    static constexpr uint8_t REPORT_UNKNOWN_79     = 0x79; //  1 byte:  TBD

    static constexpr uint8_t CHARGE_STATE_DISCHARGING   = 1;
    static constexpr uint8_t CHARGE_STATE_CHARGING      = 2;
    static constexpr uint8_t CHARGE_STATE_CHARGING_DONE = 4;

    // Feature report IDs — the command channel to the firmware.
    // Commands are wrapped inside Feature Report 0x01 (or 0x02 as fallback).
    // Buffer layout: [feature_report_id | cmd_byte | payload_size | payload...]
    static constexpr uint8_t FEATURE_REPORT_CMD  = 0x01;
    static constexpr uint8_t FEATURE_REPORT_CMD2 = 0x02;  // fallback if 0x01 fails

    // Command bytes (go in buffer[1] inside the feature report)
    static constexpr uint8_t CMD_SET_DIGITAL_MAPPINGS   = 0x80;
    static constexpr uint8_t CMD_CLEAR_DIGITAL_MAPPINGS = 0x81;  // ← lizard off
    static constexpr uint8_t CMD_GET_DIGITAL_MAPPINGS   = 0x82;
    static constexpr uint8_t CMD_SET_DEFAULT_MAPPINGS   = 0x85;  // ← lizard on
    static constexpr uint8_t CMD_SET_SETTINGS           = 0x87;
    static constexpr uint8_t CMD_GET_SETTINGS           = 0x89;
    static constexpr uint8_t CMD_LOAD_DEFAULT_SETTINGS  = 0x8E;  // ← restore settings for lizard on

    // Triton output report IDs.
    static constexpr uint8_t OUT_HAPTIC_RUMBLE   = 0x80;  // continuous two-channel haptic
    static constexpr uint8_t OUT_HAPTIC_PULSE    = 0x81;  // one-shot pulse (on/off/repeat/gain)
    static constexpr uint8_t OUT_HAPTIC_COMMAND  = 0x82;  // named command (tick/click + gain)

    // Setting key IDs (go in the payload of CMD_SET_SETTINGS)
    static constexpr uint8_t  SETTING_RIGHT_TRACKPAD_MODE = 0x07;
    static constexpr uint8_t  SETTING_LEFT_TRACKPAD_MODE  = 0x08;
    static constexpr uint8_t  SETTING_IMU_MODE            = 0x30;
    static constexpr uint8_t  TRACKPAD_NONE               = 0x00;
    static constexpr uint16_t IMU_MODE_OFF                = 0x0000;
    static constexpr uint16_t IMU_MODE_RAW_ACCEL_GYRO     = 0x0018;

    // ---------------------------------------------------------------------------
    // Input report layout — 0x45 / 0x42 STATE report (buf[0] = 0x45 or 0x42)
    // ---------------------------------------------------------------------------

    // buf[01]       — 8-bit sequence counter (wraps 0xFF → 0x00)

    // buf[02]       — button bitmask byte 0
    static constexpr uint8_t BTN_A        = 0x01;  // bit 0
    static constexpr uint8_t BTN_B        = 0x02;  // bit 1
    static constexpr uint8_t BTN_X        = 0x04;  // bit 2
    static constexpr uint8_t BTN_Y        = 0x08;  // bit 3
    // bit 4 (0x10): TBD
    static constexpr uint8_t BTN_RS       = 0x20;  // bit 5 — right stick click
    static constexpr uint8_t BTN_MENU     = 0x40;  // bit 6 — ≡ Menu / Start
    static constexpr uint8_t BTN_R4       = 0x80;  // bit 7 — back paddle R4

    // buf[03]       — button bitmask byte 1
    static constexpr uint8_t BTN_R5       = 0x01;  // bit 0 — back paddle R5
    static constexpr uint8_t BTN_RB       = 0x02;  // bit 1
    static constexpr uint8_t BTN_DPAD_DN  = 0x04;  // bit 2
    static constexpr uint8_t BTN_DPAD_RT  = 0x08;  // bit 3
    static constexpr uint8_t BTN_DPAD_LT  = 0x10;  // bit 4
    static constexpr uint8_t BTN_DPAD_UP  = 0x20;  // bit 5
    static constexpr uint8_t BTN_VIEW     = 0x40;  // bit 6 — ⧉ View / Back
    static constexpr uint8_t BTN_LS       = 0x80;  // bit 7 — left stick click

    // buf[04]       — button bitmask byte 2
    static constexpr uint8_t BTN_STEAM       = 0x01;  // bit 0 — Steam / Guide
    static constexpr uint8_t BTN_L4          = 0x02;  // bit 1 — back paddle L4
    static constexpr uint8_t BTN_L5          = 0x04;  // bit 2 — back paddle L5
    static constexpr uint8_t BTN_LB          = 0x08;  // bit 3
    static constexpr uint8_t BTN_RS_TOUCH    = 0x10;  // bit 4 — right stick capacitive touch
    static constexpr uint8_t BTN_TP_RT       = 0x20;  // bit 5 — right trackpad active (touch or click)
    static constexpr uint8_t BTN_TP_RT_CLICK = 0x40;  // bit 6 — right trackpad hard press (physical click)
    static constexpr uint8_t BTN_RT_FULL     = 0x80;  // bit 7 — right trigger fully pressed (digital threshold)

    // buf[05]       — flags byte
    static constexpr uint8_t BTN_LS_TOUCH    = 0x01;  // bit 0 — left stick capacitive touch
    static constexpr uint8_t BTN_TP_LT       = 0x02;  // bit 1 — left trackpad active (touch or click)
    static constexpr uint8_t BTN_TP_LT_CLICK = 0x04;  // bit 2 — left trackpad hard press
    // bit 3 (0x08): TBD
    static constexpr uint8_t FLAG_GRIP_RT    = 0x10;  // bit 4 — right grip sensor active
    static constexpr uint8_t FLAG_GRIP_LT    = 0x20;  // bit 5 — left grip sensor active
    // other bits TBD

    // buf[06..07]   — left trigger,  16-bit LE signed, 0x0000 (released) – 0x7FFF (full)
    // buf[08..09]   — right trigger, 16-bit LE signed, 0x0000 (released) – 0x7FFF (full)

    // buf[10..11]   — left joystick X,  16-bit LE signed; center ≈ 0x0000, +0x7FFF = right, −0x8000 = left
    // buf[12..13]   — left joystick Y,  16-bit LE signed; center ≈ 0x0000, +0x7FFF = up, −0x8000 = down
    // buf[14..15]   — right joystick X, 16-bit LE signed; center ≈ 0x0000, +0x7FFF = right, −0x8000 = left
    // buf[16..17]   — right joystick Y, 16-bit LE signed; center ≈ 0x0000, +0x7FFF = up, −0x8000 = down

    // buf[18..19]   — left trackpad X, 16-bit LE signed (0x0000 when not touching)
    // buf[20..21]   — left trackpad Y, 16-bit LE signed (0x0000 when not touching)
    // buf[22..23]   — left trackpad contact area, 16-bit LE (0 = no contact; higher = more area/pressure)

    // buf[24..25]   — right trackpad X, 16-bit LE signed (0x0000 when not touching)
    // buf[26..27]   — right trackpad Y, 16-bit LE signed (0x0000 when not touching)
    // buf[28..29]   — right trackpad contact area, 16-bit LE (0 = no contact; higher = more area/pressure)

    // IMU data — only present when IMU is enabled (SETTING_IMU_MODE = IMU_MODE_RAW_ACCEL_GYRO).
    // Report length grows to ≥ 46 bytes.
    // buf[30..33]   — IMU timestamp, 32-bit LE unsigned (units ~16 µs)
    // buf[34..35]   — accelerometer X, 16-bit LE signed
    // buf[36..37]   — accelerometer Y, 16-bit LE signed
    // buf[38..39]   — accelerometer Z, 16-bit LE signed
    // buf[40..41]   — gyroscope X, 16-bit LE signed
    // buf[42..43]   — gyroscope Y, 16-bit LE signed
    // buf[44..45]   — gyroscope Z, 16-bit LE signed

    // ---------------------------------------------------------------------------

    SteamController() = default;
    ~SteamController() { Close(); }
    SteamController(const SteamController&) = delete;
    SteamController& operator=(const SteamController&) = delete;

    // Returns paths for all live Steam Controller interfaces (probes each one).
    static std::vector<std::wstring> EnumerateAll();

    // Find and open the first live vendor HID interface.
    bool Open();
    // Open a specific path returned by EnumerateAll (no probe needed).
    bool Open(const std::wstring& path);
    void Close();
    bool IsOpen() const { return m_device.IsOpen(); }

    // Returns true for any report ID that carries controller state.
    static bool IsStateReportId(uint8_t id) {
        return id == REPORT_STATE || id == REPORT_STATE_LEGACY;
    }

    // Two-step sequence: clears digital mappings + sets trackpads to NONE.
    // Starts the background rumble thread.
    bool DisableLizardMode();

    // Restores default mappings. Should be called before process exit.
    bool EnableLizardMode();

    // Reopen the device handle with FILE_SHARE_READ only, preventing other
    // processes (e.g. Steam) from obtaining write access. Call before
    // DisableLizardMode() when entering game mode.
    bool ClaimExclusive();

    // Reopen the device handle with full share flags, allowing other processes
    // to open the device for write. Call after EnableLizardMode() when leaving
    // game mode so Steam can reclaim the controller.
    void ReleaseToShared();

    // Read the next raw input report. buffer[0] = report ID on return.
    // Returns 0 on timeout.
    size_t ReadReport(uint8_t* buffer, size_t size, uint32_t timeoutMs = 16);

    // Enable or disable raw IMU (accel + gyro) output in the state report.
    bool SetImuEnabled(bool enabled);

    // Set rumble intensity from XInput/DS4 motor bytes (0–255 each).
    // Uses a power curve, cross-channel mixing, and a short attack boost on
    // rising edges to produce a more natural feel from the haptic actuators.
    // Call with (0, 0) to stop rumble.
    void SetRumble(uint8_t largeMotor, uint8_t smallMotor);

    // Re-assert the lizard-off state (cleared mappings + raw trackpad modes).
    // The firmware silently reverts to lizard mode — including its autonomous
    // click haptics — after a period without host feature reports. Call this
    // every couple of seconds while game mode is active, like hid-steam does.
    bool SendKeepalive();

    // Crash-context lizard restore: sends the default-mappings and
    // default-settings feature reports WITHOUT taking locks or joining
    // threads, so it is safe from an unhandled-exception filter where another
    // thread may hold m_writeMutex forever. Racing a concurrent writer is
    // acceptable — if this write is lost, the firmware's own revert timeout
    // restores lizard mode a few seconds after the process dies.
    void EmergencyLizardRestore() noexcept;

    // Fire a single haptic event on one trackpad.
    // strongClick = true → physical-click sensation; false → light touch-down tick.
    void PulseTrackpadHaptic(bool left, bool strongClick);

    // Fire a very light tick while the thumb moves on the trackpad.
    // Returns false when the tick was dropped by the send rate limiter.
    bool TickTrackpadMovement(bool left);

    // Silence any in-flight trackpad haptic state (lifecycle cleanup hook).
    void ClearTrackpadHaptics();

private:
    struct RumbleFrame {
        uint16_t left  = 0;
        uint16_t right = 0;
    };

    void RumbleLoop();
    RumbleFrame CurrentRumbleFrameLocked(std::chrono::steady_clock::time_point now) const;
    bool SendRumbleOutput(uint16_t leftSpeed, uint16_t rightSpeed);
    bool SendTrackpadPulseOutput(uint8_t side, uint16_t onUs, uint16_t offUs,
                                  uint16_t repeatCount, int16_t gainDb);
    bool SendTrackpadCommandOutput(uint8_t side, uint8_t command, int8_t gainDb);

    HidDevice         m_device;
    std::thread       m_rumbleThread;
    std::atomic<bool> m_running{false};

    // Shared by the rumble thread and callers — protect with m_writeMutex.
    std::mutex        m_writeMutex;

    // Rumble state — protect with m_rumbleMutex.
    std::mutex        m_rumbleMutex;
    uint16_t          m_rumbleBaseLeft   = 0;
    uint16_t          m_rumbleBaseRight  = 0;
    uint16_t          m_rumbleBoostLeft  = 0;
    uint16_t          m_rumbleBoostRight = 0;
    std::chrono::steady_clock::time_point m_rumbleBoostUntil{};
    std::chrono::steady_clock::time_point m_lastRumbleSent{};
    uint8_t           m_lastLargeMotor   = 0;
    uint8_t           m_lastSmallMotor   = 0;
    bool              m_hasRumbleState   = false;

    // Last trackpad haptic send per side (touched only by the read-loop
    // thread, no lock needed). The firmware queues haptic waveforms — sending
    // faster than they play builds a backlog that later drains as one crunchy
    // burst. Movement ticks are dropped while the previous waveform is likely
    // still playing; click haptics always send.
    std::chrono::steady_clock::time_point m_lastTrackpadHapticLeft{};
    std::chrono::steady_clock::time_point m_lastTrackpadHapticRight{};
};
