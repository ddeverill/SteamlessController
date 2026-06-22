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
    if (m_prevL4)    SendMouseButton(MOUSEEVENTF_LEFTUP);
    if (m_prevR4)    SendMouseButton(MOUSEEVENTF_LEFTUP);
    m_touching  = false;
    m_prevClick = false;
    m_prevL4    = false;
    m_prevR4    = false;
    m_prevX     = 0;
    m_prevY     = 0;
}

void TrackpadMouse::Update(const uint8_t* buf, size_t n) {
    // L4 (upper-left paddle) and R4 (upper-right paddle) → left mouse click.
    // This works independently of trackpad mouse mode.
    if (m_backButtonsEnabled && n > 4) {
        bool l4 = (buf[4] & SteamController::BTN_L4) != 0;
        bool r4 = (n > 2) && (buf[2] & SteamController::BTN_R4) != 0;
        if (l4 != m_prevL4) {
            SendMouseButton(l4 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
            m_prevL4 = l4;
        }
        if (r4 != m_prevR4) {
            SendMouseButton(r4 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
            m_prevR4 = r4;
        }
    }

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
            INPUT input{};
            input.type       = INPUT_MOUSE;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            input.mi.dx      = static_cast<LONG>(dx * SENSITIVITY);
            input.mi.dy      = static_cast<LONG>(dy * SENSITIVITY);
            SendInput(1, &input, sizeof(INPUT));
        }
    }

    if (touching) { m_prevX = x; m_prevY = y; }
    m_touching = touching;

    if (clicking != m_prevClick) {
        SendMouseButton(clicking ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
        m_prevClick = clicking;
    }
}
