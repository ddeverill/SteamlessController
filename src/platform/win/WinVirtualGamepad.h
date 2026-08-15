#pragma once
#include "core/iface/IVirtualGamepad.h"
#include "core/BackButtonConfig.h"
#include "core/ControllerPlatform.h"
#include "core/TrackpadConfig.h"
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include <thread>

class WinVirtualGamepad : public IVirtualGamepad {
public:
    WinVirtualGamepad(ControllerPlatform platform, RumbleCallback rumbleCallback);
    ~WinVirtualGamepad() override;
    WinVirtualGamepad(const WinVirtualGamepad&) = delete;
    WinVirtualGamepad& operator=(const WinVirtualGamepad&) = delete;

    bool IsValid()        const override { return m_valid; }
    // Established by asking the system what buses exist, not inferred from an
    // error code — VIGEM_ERROR_BUS_NOT_FOUND is returned for causes other than
    // an absent driver, and reporting those as "driver missing" is what sent
    // the reporter in #65 looking for a driver they already had.
    bool IsDriverMissing() const override { return m_driverMissing; }

    const char*  FailStage() const override { return m_failStage; }
    uint32_t     LastError() const override { return m_lastError; }
    std::string  BusReport() const override { return m_busReport; }

    void Update(const uint8_t* buf, size_t n, const ControllerProfile& profile) override;
    void SetBatteryState(uint8_t levelPercent, uint8_t chargeState) override;

    // Called by the ViGEm rumble notification thunk / the DS4 output thread.
    void OnRumble(uint8_t largeMotor, uint8_t smallMotor);

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
    std::string m_busReport;

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

class WinVirtualGamepadFactory : public IVirtualGamepadFactory {
public:
    std::unique_ptr<IVirtualGamepad> Create(ControllerPlatform platform,
                                             RumbleCallback rumbleCallback) override;
    bool        BusAvailable() const override;
    std::string BusReport()   const override;
};
