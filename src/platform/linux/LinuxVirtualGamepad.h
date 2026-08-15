#pragma once
#include "core/iface/IVirtualGamepad.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

// A uinput-backed virtual gamepad, shaped like a wired Xbox 360 controller
// (vendor/product 045e:028e — the well-known GUID SDL2/Steam Input already
// ship a mapping for, so games get correct button semantics without relying
// on evdev-capability auto-mapping heuristics).
//
// DualShock 4 mode is not offered here: uinput is evdev-level and cannot
// carry the DS4's touchpad contacts, gyro or lightbar — those need
// /dev/uhid (a real virtual HID device with its own report descriptor,
// letting the kernel's hid-playstation driver bind it), which is a
// materially bigger effort and root-only by default on most distros. A
// request for ControllerPlatform::PlayStation is honored by falling back to
// the same Xbox 360 shape, cosmetically relabeled — see the .cpp.
class LinuxVirtualGamepad : public IVirtualGamepad {
public:
    LinuxVirtualGamepad(ControllerPlatform platform, RumbleCallback rumbleCallback);
    ~LinuxVirtualGamepad() override;
    LinuxVirtualGamepad(const LinuxVirtualGamepad&) = delete;
    LinuxVirtualGamepad& operator=(const LinuxVirtualGamepad&) = delete;

    bool IsValid()        const override { return m_valid; }
    bool IsDriverMissing() const override { return m_driverMissing; }
    const char*  FailStage() const override { return m_failStage; }
    uint32_t     LastError() const override { return m_lastError; }
    std::string  BusReport() const override { return m_busReport; }

    void Update(const uint8_t* buf, size_t n, const ControllerProfile& profile) override;
    // DS4-only on Windows; no-op here (no DS4 target to report battery on).
    void SetBatteryState(uint8_t levelPercent, uint8_t chargeState) override;

private:
    void FfLoop();
    void HandleFfUpload();
    void HandleFfErase();
    void HandlePlay(int effectId, int repeatCount);
    void StopRumbleAfterDuration();

    ControllerPlatform m_platform;
    RumbleCallback      m_rumbleCallback;
    int                 m_fd            = -1;
    bool                m_valid         = false;
    bool                m_driverMissing = false;
    const char*         m_failStage     = "none";
    uint32_t            m_lastError     = 0;
    std::string         m_busReport;

    // One uploaded FF_RUMBLE effect at a time is all SDL/Steam Input ever
    // use on Linux; track just that one rather than a general effect table.
    struct RumbleEffect {
        uint16_t strong = 0, weak = 0;
        uint32_t lengthMs = 0;
    };
    RumbleEffect m_effect;
    bool         m_haveEffect = false;

    std::thread       m_ffThread;
    std::atomic<bool> m_ffRunning{false};

    // Duration-based auto-stop for the currently playing effect (evdev FF
    // effects have a `replay.length` the kernel does not enforce for a
    // uinput passthrough device — something has to time it out).
    std::thread            m_stopThread;
    std::atomic<uint64_t>  m_playGeneration{0};
};

class LinuxVirtualGamepadFactory : public IVirtualGamepadFactory {
public:
    std::unique_ptr<IVirtualGamepad> Create(ControllerPlatform platform,
                                             RumbleCallback rumbleCallback) override;
    bool        BusAvailable() const override;
    std::string BusReport() const override;
};
