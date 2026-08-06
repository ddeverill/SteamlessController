#pragma once
#include <cstdint>
#include <cstddef>

class TrackpadMouse {
public:
    void SetTrackpadEnabled(bool enabled)   { m_trackpadEnabled  = enabled; }
    void SetUseLeftTrackpad(bool enabled)   { m_useLeftTrackpad  = enabled; }

    void Update(const uint8_t* buf, size_t n);
    void Reset();

private:
    bool     m_trackpadEnabled   = false;
    bool     m_useLeftTrackpad   = false;

    bool     m_touching  = false;
    bool     m_prevClick = false;
    int16_t  m_prevX     = 0;
    int16_t  m_prevY     = 0;
    float    m_remX      = 0.0f;  // sub-pixel movement carry
    float    m_remY      = 0.0f;

    static constexpr float SENSITIVITY = 0.01125f;
};
