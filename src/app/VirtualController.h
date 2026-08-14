#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include "BackButtonConfig.h"
#include "ControllerPlatform.h"
#include "TrackpadConfig.h"

class VirtualController {
public:
    using RumbleCallback = std::function<void(uint8_t largeMotor, uint8_t smallMotor)>;

    explicit VirtualController(ControllerPlatform platform, RumbleCallback rumbleCallback = {});
    ~VirtualController();
    VirtualController(const VirtualController&) = delete;
    VirtualController& operator=(const VirtualController&) = delete;

    bool IsValid()        const { return m_valid; }
    // Established by asking the system what buses exist, not inferred from an
    // error code — VIGEM_ERROR_BUS_NOT_FOUND is returned for causes other than
    // an absent driver, and reporting those as "driver missing" is what sent
    // the reporter in #65 looking for a driver they already had.
    bool IsDriverMissing() const { return m_driverMissing; }

    // Why construction failed. Only meaningful when !IsValid(); FailStage() names
    // the ViGEm call that failed and LastError() carries its VIGEM_ERROR code
    // (0 when the failing call returns no code, e.g. the allocators).
    const char* FailStage() const { return m_failStage; }
    uint32_t    LastError() const { return m_lastError; }

    // What buses were on the machine when construction failed, for the log and
    // for choosing what to tell the user. Empty unless a connect failed.
    const std::wstring& BusReport() const { return m_busReport; }

    void Update(const uint8_t* buf, size_t n, const ControllerProfile& profile);
    void OnRumble(uint8_t largeMotor, uint8_t smallMotor);

    // DS4-only: update battery level shown in the DS4 report.
    void SetBatteryState(uint8_t levelPercent, uint8_t chargeState);

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
    const char* m_failStage = "none";
    uint32_t    m_lastError = 0;
    std::wstring m_busReport;

    // DS4 touchpad tracking. Which pads feed it is read from the profile
    // handed to Update every frame, so none of that is duplicated here.
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
