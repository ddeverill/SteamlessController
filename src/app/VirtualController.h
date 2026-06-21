#pragma once
#include <cstdint>
#include <cstddef>
#include "BackButtonConfig.h"

class VirtualController {
public:
    VirtualController();
    ~VirtualController();
    VirtualController(const VirtualController&) = delete;
    VirtualController& operator=(const VirtualController&) = delete;

    bool IsValid()          const { return m_valid; }
    bool IsDriverMissing()  const { return m_driverMissing; }

    void Update(const uint8_t* buf, size_t n, const BackButtonConfig& backCfg, bool backMouseEnabled);

private:
    void* m_client       = nullptr;
    void* m_target       = nullptr;
    bool  m_valid        = false;
    bool  m_driverMissing = false;
};
