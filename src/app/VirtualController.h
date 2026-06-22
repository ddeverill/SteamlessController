#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <functional>
#include <thread>
#include "BackButtonConfig.h"
#include "ControllerPlatform.h"

class VirtualController {
public:
    using RumbleCallback = std::function<void(uint8_t largeMotor, uint8_t smallMotor)>;

    explicit VirtualController(ControllerPlatform platform, RumbleCallback rumbleCallback = {});
    ~VirtualController();
    VirtualController(const VirtualController&) = delete;
    VirtualController& operator=(const VirtualController&) = delete;

    bool IsValid()        const { return m_valid; }
    bool IsDriverMissing() const { return m_driverMissing; }

    void Update(const uint8_t* buf, size_t n, const BackButtonConfig& backCfg, bool backMouseEnabled);
    void OnRumble(uint8_t largeMotor, uint8_t smallMotor);

    // DS4-only: update battery level shown in the DS4 report.
    void SetBatteryState(uint8_t levelPercent, uint8_t chargeState);

    // Tell VirtualController which (if any) trackpad is claimed for mouse use,
    // so that pad's touch data is excluded from the DS4 touchpad report.
    void SetTrackpadMouseClaim(bool enabled, bool useLeftTrackpad);

private:
    void StartDs4OutputThread();
    void StopDs4OutputThread();
    void Ds4OutputLoop();

    ControllerPlatform m_platform;
    void*  m_client       = nullptr;
    void*  m_target       = nullptr;
    RumbleCallback m_rumbleCallback;
    bool   m_valid        = false;
    bool   m_driverMissing = false;

    // DS4 touchpad tracking
    bool     m_trackpadMouseEnabled    = false;
    bool     m_useLeftTrackpadForMouse = false;
    uint8_t  m_touchPacketCounter      = 0;
    uint8_t  m_rightTracking           = 0;
    uint8_t  m_leftTracking            = 0;
    bool     m_wasRightTouching        = false;
    bool     m_wasLeftTouching         = false;

    // DS4 IMU timestamp tracking
    uint16_t m_ds4Timestamp        = 0;
    uint32_t m_lastImuTimestamp    = 0;
    bool     m_hasLastImuTimestamp = false;

    // DS4 battery
    uint8_t m_ds4BatteryLevel   = 0x0B;  // full / unknown
    uint8_t m_ds4BatterySpecial = 0x1B;

    // DS4 output thread (polls for rumble/lightbar output reports)
    std::atomic<bool> m_ds4OutputRunning{false};
    std::thread       m_ds4OutputThread;
};
