#include "TrackpadMouse.h"
#include "InputInjection.h"
#include "steam/SteamController.h"
#include <Windows.h>
#include <cstdlib>
#include <cstring>
#include <cstdint>

void TrackpadMouse::SetMode(TrackpadMode mode) {
    if (mode == m_mode) return;
    m_mode = mode;
    Reset();  // carried remainders and touch state mean nothing to the new mode
}

void TrackpadMouse::Reset() {
    m_touching   = false;
    m_prevX      = 0;
    m_prevY      = 0;
    m_remX       = 0.0f;
    m_remY       = 0.0f;
    m_scrollRemX = 0.0f;
    m_scrollRemY = 0.0f;
    m_haveRef    = false;
    m_travelSent = 0;
}

// Tracks whether accepted movement is reaching the cursor. Re-arms whenever the
// cursor does move, so only a genuinely pinned cursor accumulates travel.
void TrackpadMouse::NoteMovementSent(long px, bool haveCursor,
                                     long cursorX, long cursorY) {
    if (!haveCursor) {
        // No cursor position to compare against — GetCursorPos itself fails
        // off the input desktop, and InputInjection reports that separately.
        m_haveRef    = false;
        m_travelSent = 0;
        return;
    }

    if (!m_haveRef || cursorX != m_refCursorX || cursorY != m_refCursorY) {
        m_refCursorX = cursorX;
        m_refCursorY = cursorY;
        m_haveRef    = true;
        m_travelSent = 0;
    }

    m_travelSent += px;
    if (m_travelSent >= STUCK_TRAVEL_PX) {
        POINT at{ m_refCursorX, m_refCursorY };
        InputInjection::LogCursorNotMoving(m_travelSent, at);
        m_travelSent = 0;  // re-arm; the log call rate-limits itself
    }
}

void TrackpadMouse::UpdatePointer(int dx, int dy) {
    // Carry sub-pixel remainders between frames — per-frame deltas scaled by
    // sensitivity are often below one pixel, and truncating them each frame
    // would discard slow movement entirely.
    const float fx = static_cast<float>(dx) * SENSITIVITY + m_remX;
    const float fy = static_cast<float>(dy) * SENSITIVITY + m_remY;
    const LONG  ix = static_cast<LONG>(fx);
    const LONG  iy = static_cast<LONG>(fy);
    m_remX = fx - static_cast<float>(ix);
    m_remY = fy - static_cast<float>(iy);
    if (ix == 0 && iy == 0) return;

    INPUT input{};
    input.type       = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx      = ix;
    input.mi.dy      = iy;
    // Read the cursor before sending: SendInput queues the event rather than
    // applying it, so a position read straight after would still be the old one.
    POINT      before{};
    const bool haveBefore = GetCursorPos(&before) != FALSE;
    if (InputInjection::Send(input, "trackpad-move")) {
        NoteMovementSent(std::labs(ix) + std::labs(iy),
                         haveBefore, before.x, before.y);
    }
}

// Wheel events are quantised to WHEEL_DELTA notches, which is much coarser
// than a pad frame's movement — so the same remainder-carry the pointer path
// uses is what makes slow scrolling work at all here.
//
// Takes raw pad deltas (Y growing upward). Natural scrolling means the
// content follows the finger, which is the opposite sense to the wheel's own
// convention that positive is "away from the user" — hence the inversion
// here rather than at the call site.
void TrackpadMouse::UpdateScroll(int dx, int dy) {
    if (m_scrollDir == ScrollDirection::Natural) { dx = -dx; dy = -dy; }

    const float fy = static_cast<float>(dy) * SCROLL_SENSITIVITY + m_scrollRemY;
    const float fx = static_cast<float>(dx) * SCROLL_SENSITIVITY + m_scrollRemX;
    const LONG  iy = static_cast<LONG>(fy);
    const LONG  ix = static_cast<LONG>(fx);
    m_scrollRemY = fy - static_cast<float>(iy);
    m_scrollRemX = fx - static_cast<float>(ix);

    if (iy != 0) {
        INPUT input{};
        input.type         = INPUT_MOUSE;
        input.mi.dwFlags   = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(iy);
        InputInjection::Send(input, "trackpad-scroll");
    }
    if (ix != 0) {
        INPUT input{};
        input.type         = INPUT_MOUSE;
        input.mi.dwFlags   = MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = static_cast<DWORD>(ix);
        InputInjection::Send(input, "trackpad-hscroll");
    }
}

void TrackpadMouse::Update(const uint8_t* buf, size_t n) {
    // Pad clicks are handled by ControllerManager, alongside every other
    // binding — this is movement only. Only the two desktop-driving modes
    // produce anything here; None and DS4Touchpad are both "not the desktop's".
    if (n < 30
        || (m_mode != TrackpadMode::MousePointer && m_mode != TrackpadMode::ScrollWheel))
        return;

    const uint8_t b2 = buf[4];
    const uint8_t b3 = buf[5];

    const bool touching = m_isLeftPad
        ? (b3 & SteamController::BTN_TP_LT) != 0
        : (b2 & SteamController::BTN_TP_RT) != 0;

    int16_t x = 0, y = 0;
    if (m_isLeftPad) {
        memcpy(&x, buf + 18, 2);
        memcpy(&y, buf + 20, 2);
    } else {
        memcpy(&x, buf + 24, 2);
        memcpy(&y, buf + 26, 2);
    }

    if (touching && m_touching) {
        const int dxRaw = static_cast<int>(x - m_prevX);
        const int dyRaw = static_cast<int>(y - m_prevY);
        if (dxRaw != 0 || dyRaw != 0) {
            if (m_mode == TrackpadMode::MousePointer) {
                // Pad Y grows upward, screen Y grows downward.
                UpdatePointer(dxRaw, -dyRaw);
            } else {
                // Raw deltas — UpdateScroll owns the direction convention.
                UpdateScroll(dxRaw, dyRaw);
            }
        }
    }

    if (touching) { m_prevX = x; m_prevY = y; }
    m_touching = touching;
}
