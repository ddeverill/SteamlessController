#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include "core/ControllerPlatform.h"

struct ControllerProfile;

// One virtual gamepad target (Windows: a ViGEm X360/DS4 target; Linux: a
// uinput X360-shaped pad — see platform/linux/LinuxVirtualGamepad.h for why
// DS4 is not offered there in v1).
class IVirtualGamepad {
public:
    virtual ~IVirtualGamepad() = default;

    virtual bool IsValid() const = 0;
    // Established by asking the system what buses/devices exist, not
    // inferred from an error code alone — see VirtualController::IsDriverMissing
    // for why that distinction matters (issue #65).
    virtual bool IsDriverMissing() const = 0;

    virtual const char*  FailStage() const = 0;
    virtual uint32_t     LastError() const = 0;
    virtual std::string  BusReport() const = 0;

    virtual void Update(const uint8_t* buf, size_t n, const ControllerProfile& profile) = 0;

    // DS4-only: update the battery level shown in the DS4 report. No-op on
    // backends that don't expose a DS4 target.
    virtual void SetBatteryState(uint8_t levelPercent, uint8_t chargeState) = 0;
};

using RumbleCallback = std::function<void(uint8_t largeMotor, uint8_t smallMotor)>;

class IVirtualGamepadFactory {
public:
    virtual ~IVirtualGamepadFactory() = default;

    virtual std::unique_ptr<IVirtualGamepad> Create(ControllerPlatform platform,
                                                     RumbleCallback rumbleCallback) = 0;

    // Whether the underlying virtual-gamepad mechanism is present at all
    // (Windows: ViGEmBus driver; Linux: /dev/uinput exists and is writable).
    // Checked before attempting Create() so "driver missing" can be reported
    // without spending a create/destroy cycle on the real device.
    virtual bool        BusAvailable() const = 0;
    virtual std::string BusReport() const = 0;
};
