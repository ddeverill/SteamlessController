#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>

class VirtualController {
public:
    using RumbleCallback = std::function<void(uint8_t largeMotor, uint8_t smallMotor)>;

    explicit VirtualController(RumbleCallback rumbleCallback = {});
    ~VirtualController();
    VirtualController(const VirtualController&) = delete;
    VirtualController& operator=(const VirtualController&) = delete;

    bool IsValid()          const { return m_valid; }
    bool IsDriverMissing()  const { return m_driverMissing; }

    void Update(const uint8_t* buf, size_t n);
    void OnRumble(uint8_t largeMotor, uint8_t smallMotor);

private:
    void* m_client       = nullptr;
    void* m_target       = nullptr;
    RumbleCallback m_rumbleCallback;
    bool  m_valid        = false;
    bool  m_driverMissing = false;
};
