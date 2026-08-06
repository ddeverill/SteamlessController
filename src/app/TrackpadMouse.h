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

    // Movement Windows accepted, checked against the cursor actually moving.
    // A refused SendInput reports itself; motion that is accepted and then
    // discarded — by a cursor clip a game left behind, or a low-level hook
    // filtering injected input — looks identical to the user and otherwise
    // leaves no trace at all. Records where the cursor was and how much travel
    // has been sent since; when the travel adds up and the position has not
    // changed, that is worth a log line.
    long     m_refCursorX  = 0;
    long     m_refCursorY  = 0;
    bool     m_haveRef     = false;
    long     m_travelSent  = 0;

    // Enough sent movement that a still cursor cannot be a coincidence.
    static constexpr long  STUCK_TRAVEL_PX = 120;

    static constexpr float SENSITIVITY = 0.01125f;

    void NoteMovementSent(long px, bool haveCursor, long cursorX, long cursorY);
};
