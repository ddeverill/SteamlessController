#pragma once
#include <cstdint>
#include <cstddef>
#include "TrackpadConfig.h"

// Desktop input driven by ONE physical trackpad. One instance per pad, so the
// two pads can be configured independently — this used to be a single instance
// switched between pads by a bool, which structurally could not represent
// "both pads doing something".
//
// Only movement lives here. The pad's click is a binding like any other and is
// dispatched by ControllerManager alongside the back paddles, so it works the
// same whatever this pad's movement mode is.
class TrackpadMouse {
public:
    // Which physical pad to read. Set once when the slot is created.
    void SetPad(bool isLeftPad) { m_isLeftPad = isLeftPad; }
    void SetMode(TrackpadMode mode);
    void SetScrollDirection(ScrollDirection dir) { m_scrollDir = dir; }

    void Update(const uint8_t* buf, size_t n);
    void Reset();

private:
    void UpdatePointer(int dx, int dy);
    void UpdateScroll(int dx, int dy);
    void NoteMovementSent(long px, bool haveCursor, long cursorX, long cursorY);

    bool            m_isLeftPad = false;
    TrackpadMode    m_mode      = TrackpadMode::None;
    ScrollDirection m_scrollDir = ScrollDirection::Natural;

    bool     m_touching  = false;
    int16_t  m_prevX     = 0;
    int16_t  m_prevY     = 0;
    float    m_remX      = 0.0f;  // sub-pixel movement carry
    float    m_remY      = 0.0f;
    float    m_scrollRemX = 0.0f;  // sub-detent scroll carry
    float    m_scrollRemY = 0.0f;

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
    // Chosen so a full swipe across the pad is a few notches rather than a
    // page-length fling: the pad's axes span roughly +/-32000, and one wheel
    // detent is WHEEL_DELTA (120).
    static constexpr float SCROLL_SENSITIVITY = 0.02f;
};
