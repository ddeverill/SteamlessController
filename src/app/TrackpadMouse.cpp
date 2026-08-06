#include "TrackpadMouse.h"
#include "steam/SteamController.h"
#include <Windows.h>
#include <cstring>
#include <cstdint>

static void SendMouseButton(DWORD flags) {
    INPUT input{};
    input.type       = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    SendInput(1, &input, sizeof(INPUT));
}

void TrackpadMouse::Reset() {
    if (m_prevClick) SendMouseButton(MOUSEEVENTF_LEFTUP);
    m_touching  = false;
    m_prevClick = false;
    m_prevX     = 0;
    m_prevY     = 0;
    m_remX      = 0.0f;
    m_remY      = 0.0f;
}

void TrackpadMouse::Update(const uint8_t* buf, size_t n) {
    // Back paddles mapped to a mouse button are handled by ControllerManager,
    // alongside every other paddle binding.
    if (n < 30 || !m_trackpadEnabled) return;

    const uint8_t b2 = buf[4];
    const uint8_t b3 = buf[5];

    const bool touching = m_useLeftTrackpad
        ? (b3 & SteamController::BTN_TP_LT)       != 0
        : (b2 & SteamController::BTN_TP_RT)        != 0;
    const bool clicking = m_useLeftTrackpad
        ? (b3 & SteamController::BTN_TP_LT_CLICK)  != 0
        : (b2 & SteamController::BTN_TP_RT_CLICK)   != 0;

    int16_t x = 0, y = 0;
    if (m_useLeftTrackpad) {
        memcpy(&x, buf + 18, 2);
        memcpy(&y, buf + 20, 2);
    } else {
        memcpy(&x, buf + 24, 2);
        memcpy(&y, buf + 26, 2);
    }

    if (touching && m_touching) {
        const int dx =  static_cast<int>(x - m_prevX);
        const int dy = -static_cast<int>(y - m_prevY);
        if (dx != 0 || dy != 0) {
            // Carry sub-pixel remainders between frames — per-frame deltas
            // scaled by sensitivity are often below one pixel, and truncating
            // them each frame would discard slow movement entirely.
            const float fx = static_cast<float>(dx) * SENSITIVITY + m_remX;
            const float fy = static_cast<float>(dy) * SENSITIVITY + m_remY;
            const LONG  ix = static_cast<LONG>(fx);
            const LONG  iy = static_cast<LONG>(fy);
            m_remX = fx - static_cast<float>(ix);
            m_remY = fy - static_cast<float>(iy);
            if (ix != 0 || iy != 0) {
                INPUT input{};
                input.type       = INPUT_MOUSE;
                input.mi.dwFlags = MOUSEEVENTF_MOVE;
                input.mi.dx      = ix;
                input.mi.dy      = iy;
                SendInput(1, &input, sizeof(INPUT));
            }
        }
    }

    if (touching) { m_prevX = x; m_prevY = y; }
    m_touching = touching;

    if (clicking != m_prevClick) {
        SendMouseButton(clicking ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
        m_prevClick = clicking;
    }
}
