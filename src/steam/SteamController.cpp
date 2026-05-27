#include "SteamController.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

// ---------------------------------------------------------------------------
// Internal helper: build a 64-byte feature report command buffer.
//
// The 2026 Steam Controller routes firmware commands through Feature Report
// 0x01 (same channel the original SC used, now with an explicit report ID).
//
// Buffer layout:
//   [0] FEATURE_REPORT_CMD (0x01)  — HID feature report ID
//   [1] cmd                         — command byte (0x81, 0x87, etc.)
//   [2] payloadSize                 — number of payload bytes that follow
//   [3..3+payloadSize-1] payload    — command arguments
//   [rest] zeros
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

// ---------------------------------------------------------------------------
// Open / Close
// ---------------------------------------------------------------------------

bool SteamController::Open() {
    for (uint16_t pid : { SC2026_PID, SC2026_DONGLE_PID }) {
        auto paths = HidDevice::Enumerate(VALVE_VID, pid, VENDOR_USAGE_PAGE);
        if (paths.empty()) continue;

        // For the wired controller there is only one interface; for the dongle
        // there are up to four slots (one per paired controller). Try each in
        // order and use the first that produces a live input report.
        for (auto const& path : paths) {
            if (!m_device.Open(path)) continue;

            uint8_t buf[64];
            size_t n = m_device.ReadInputReport(buf, sizeof(buf), /*timeoutMs=*/500);
            if (n > 0 && buf[0] == REPORT_STATE) {
                printf("Active interface found for PID=%04X.\n", pid);
                return true;
            }

            if (n > 0)
                printf("Unexpected report ID 0x%02X on PID=%04X (expected 0x%02X) — possible firmware mismatch.\n",
                       buf[0], pid, REPORT_STATE);
            else
                printf("Read timeout on PID=%04X vendor interface — no active controller on this slot.\n", pid);

            m_device.Close();
        }
    }

    printf("No Steam Controller found (wired PID=%04X or dongle PID=%04X).\n",
           SC2026_PID, SC2026_DONGLE_PID);
    return false;
}

void SteamController::Close() {
    if (m_running.exchange(false) && m_heartbeat.joinable())
        m_heartbeat.join();
    m_device.Close();
}

// ---------------------------------------------------------------------------
// Lizard mode
// ---------------------------------------------------------------------------

bool SteamController::DisableLizardMode() {
    uint8_t buf[64];

    // Step 1: CLEAR_DIGITAL_MAPPINGS — kills keyboard/mouse button emulation.
    BuildCmd(buf, CMD_CLEAR_DIGITAL_MAPPINGS);
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        if (!m_device.SendFeatureReport(buf, sizeof(buf))) {
            printf("Failed to send CLEAR_DIGITAL_MAPPINGS.\n");
            return false;
        }
    }

    // Step 2: SET_SETTINGS — set both trackpads to TRACKPAD_NONE.
    // Payload: pairs of [setting_id, val_lo, val_hi].
    const uint8_t settingsPayload[] = {
        SETTING_LEFT_TRACKPAD_MODE,  0x00, 0x00,
        SETTING_RIGHT_TRACKPAD_MODE, 0x00, 0x00,
    };
    BuildCmd(buf, CMD_SET_SETTINGS, settingsPayload, sizeof(settingsPayload));
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);
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

    SetRumble(0, 0);

    uint8_t buf[64];
    BuildCmd(buf, CMD_SET_DEFAULT_MAPPINGS);
    std::lock_guard<std::mutex> lock(m_commandMutex);
    return m_device.SendFeatureReport(buf, sizeof(buf));
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

size_t SteamController::ReadReport(uint8_t* buffer, size_t size, uint32_t timeoutMs) {
    return m_device.ReadInputReport(buffer, size, timeoutMs);
}

bool SteamController::SetRumble(uint16_t leftSpeed, uint16_t rightSpeed) {
    if (!m_device.IsOpen()) return false;

    m_rumbleLeft.store(leftSpeed);
    m_rumbleRight.store(rightSpeed);
    return SendRumbleOutput(leftSpeed, rightSpeed);
}

bool SteamController::SendRumbleOutput(uint16_t leftSpeed, uint16_t rightSpeed) {
    const uint16_t intensity = 0;
    const uint8_t report[] = {
        OUT_REPORT_HAPTIC_RUMBLE,
        0x00,
        static_cast<uint8_t>(intensity & 0xFF),
        static_cast<uint8_t>(intensity >> 8),
        static_cast<uint8_t>(leftSpeed & 0xFF),
        static_cast<uint8_t>(leftSpeed >> 8),
        0x00,
        static_cast<uint8_t>(rightSpeed & 0xFF),
        static_cast<uint8_t>(rightSpeed >> 8),
        0x00,
    };

    std::lock_guard<std::mutex> lock(m_commandMutex);
    return m_device.WriteOutputReport(report, sizeof(report));
}


// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

void SteamController::HeartbeatLoop() {
    uint8_t buf[64];
    BuildCmd(buf, CMD_CLEAR_DIGITAL_MAPPINGS);
    int lizardTicks = 0;

    while (m_running.load()) {
        if (lizardTicks <= 0) {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            m_device.SendFeatureReport(buf, sizeof(buf));
            lizardTicks = 20;
        }

        uint16_t leftSpeed = m_rumbleLeft.load();
        uint16_t rightSpeed = m_rumbleRight.load();
        if (leftSpeed || rightSpeed)
            SendRumbleOutput(leftSpeed, rightSpeed);

        --lizardTicks;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
}
