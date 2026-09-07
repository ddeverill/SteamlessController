#include "TrackpadInput.h"
#include "EventLog.h"
#include "InputInjection.h"
#include "steam/SteamController.h"
#include <Windows.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>

void TrackpadInput::SetMode(TrackpadMode mode) {
    if (mode == m_mode) return;
    m_mode = mode;
    Reset();  // carried remainders and touch state mean nothing to the new mode
}

void TrackpadInput::SetDiagonals(DiagonalMode d) {
    if (d == m_diagonals) return;
    m_diagonals = d;
    // The sector numbering means something different either side of this, so a
    // held one cannot carry over. Dropping it re-resolves against the new
    // geometry on the next frame; the directions it produced are released by
    // the same edge detection that handles any other change.
    m_sector = -1;
    m_dirs   = DirNone;
}

void TrackpadInput::Reset() {
    m_touching   = false;
    m_prevX      = 0;
    m_prevY      = 0;
    m_remX       = 0.0f;
    m_remY       = 0.0f;
    m_scrollRemX = 0.0f;
    m_scrollRemY = 0.0f;
    m_haveRef    = false;
    m_travelSent = 0;

    m_dirs           = DirNone;
    m_sector         = -1;
    m_clickHeld      = false;
    m_clickInCentre  = true;
}

// Tracks whether accepted movement is reaching the cursor. Re-arms whenever the
// cursor does move, so only a genuinely pinned cursor accumulates travel.
void TrackpadInput::NoteMovementSent(long px, bool haveCursor,
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

void TrackpadInput::UpdatePointer(int dx, int dy) {
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

// The wheel units one frame of movement is worth.
//
// Two corrections sit on top of the built-in scale, and they answer different
// questions. The first is not a preference at all: Windows multiplies every
// notch we send by the machine's lines-per-notch setting before anything
// moves, so the same swipe scrolls three times as far on a machine set to 10
// as on one left at the default 3. Dividing it back out is what makes the pad
// behave the same everywhere, and it is why a scroll-is-far-too-fast report
// can be true on one machine and not reproduce on another with identical code.
//
// The second is the user's own scroll speed, which is taste and nothing else.
// It is applied after the correction so the percentage means the same thing on
// every machine: 100% is the calibrated feel, not "whatever this machine does".
float TrackpadInput::ScrollScale() const {
    const float correction =
        InputInjection::WheelCorrection(InputInjection::WheelLinesPerNotch());
    return SCROLL_SENSITIVITY * correction
         * (static_cast<float>(m_scrollSpeed) / 100.0f);
}

// Wheel events are quantised to WHEEL_DELTA notches, which is much coarser
// than a pad frame's movement — so the same remainder-carry the pointer path
// uses is what makes slow scrolling work at all here.
//
// Takes raw pad deltas (Y growing upward). Natural scrolling means the
// content follows the finger, which is the opposite sense to the wheel's own
// convention that positive is "away from the user" — hence the inversion
// here rather than at the call site.
void TrackpadInput::UpdateScroll(int dx, int dy) {
    if (m_scrollDir == ScrollDirection::Natural) { dx = -dx; dy = -dy; }

    // The remainder carries in wheel units, which is what lets the scale change
    // underneath it — a slider moved mid-swipe, or the system setting changing
    // — without the part already banked meaning something different.
    const float scale = ScrollScale();
    const float fy = static_cast<float>(dy) * scale + m_scrollRemY;
    const float fx = static_cast<float>(dx) * scale + m_scrollRemX;
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

// ---------------------------------------------------------------------------
// Directional pad
// ---------------------------------------------------------------------------

// Shortest angular distance between two bearings, in degrees, always 0..180.
static double AngleDelta(double a, double b) {
    const double d = std::fmod(std::fabs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

// Which directions a sector presses. Eight-way diagonals press two, which is
// what a real d-pad does and what makes diagonal movement in a game possible;
// four-way rounds to the nearer single direction.
static uint8_t SectorDirs(int sector, DiagonalMode diagonals) {
    if (sector < 0) return DirNone;
    if (diagonals == DiagonalMode::FourWay) {
        switch (sector) {
        case 0:  return DirRight;
        case 1:  return DirUp;
        case 2:  return DirLeft;
        default: return DirDown;
        }
    }
    switch (sector) {
    case 0:  return DirRight;
    case 1:  return DirUp   | DirRight;
    case 2:  return DirUp;
    case 3:  return DirUp   | DirLeft;
    case 4:  return DirLeft;
    case 5:  return DirDown | DirLeft;
    case 6:  return DirDown;
    default: return DirDown | DirRight;
    }
}

int TrackpadInput::ResolveSector(double angleDeg) const {
    const bool   four  = m_diagonals == DiagonalMode::FourWay;
    const int    count = four ? 4 : 8;
    const double step  = four ? 90.0 : 45.0;
    const double half  = step / 2.0;

    // Hold the sector already being pressed until the thumb has cleared its
    // boundary by the hysteresis margin, so a thumb resting on a boundary
    // stays where it is instead of alternating.
    if (m_sector >= 0
            && AngleDelta(angleDeg, static_cast<double>(m_sector) * step)
                   <= half + SECTOR_HYSTERESIS_DEG)
        return m_sector;

    int s = static_cast<int>(std::floor((angleDeg + half) / step)) % count;
    if (s < 0) s += count;
    return s;
}

// Directions come from the click rather than from touch, so that resting a
// thumb on the pad presses nothing, and so the pad's three signals stay
// disjoint: touch is not zoned, a click in the middle is the click binding,
// and a click out in the ring is a direction.
void TrackpadInput::UpdateDirections(bool clicked, int16_t x, int16_t y) {
    if (!clicked) {
        m_clickHeld = false;
        m_sector    = -1;
        m_dirs      = DirNone;
        return;
    }

    const double r = std::hypot(static_cast<double>(x), static_cast<double>(y));

    if (!m_clickHeld) {
        // Rising edge. Decide the zone once, here, from the position at the
        // click itself — measurement says the centroid moves ~120 units in the
        // frame before a click and ~350 across three, against a boundary of
        // 12000 and a gap of nearly 12000 between deliberate centre and
        // deliberate direction presses. Looking back for a cleaner position
        // would be machinery for a correction two orders of magnitude smaller
        // than the thing it corrects.
        m_clickHeld     = true;
        m_clickInCentre = r < kPadRingRadius;
        m_sector        = -1;
    }

    if (m_clickInCentre) {
        m_dirs = DirNone;   // this press is the click binding, not a direction
        return;
    }

    // A ring press keeps following the thumb around the ring, which is what
    // lets a roll from up to right register as both in turn and what gives the
    // direction-change haptic something to fire on. It stops re-resolving once
    // the thumb slides inside the boundary: the zone is latched, so this press
    // can never become a centre click, and the angle gets noisy near the
    // middle where a small wobble spans whole sectors.
    if (r >= kPadRingRadius) {
        double angle = std::atan2(static_cast<double>(y), static_cast<double>(x))
                     * 180.0 / 3.14159265358979323846;
        if (angle < 0.0) angle += 360.0;
        m_sector = ResolveSector(angle);
    }
    m_dirs = SectorDirs(m_sector, m_diagonals);
}

// Reports a single frame that moved further than a finger plausibly can, which
// is what "I barely touch the pad and it scrolls a whole page" would look like
// from in here.
//
// Movement is integrated from position deltas, so dropped reports are harmless
// on their own: the displacement still adds up to the same total whether it
// arrives in ten frames or one. The case that is NOT harmless is a lift and a
// re-placement that we never saw the touch bit go false across — then the gap
// between two unrelated points is applied as one enormous swipe.
//
// The two are told apart by the time attached to each line rather than by the
// distance. Milliseconds since the previous touched frame at the report rate
// means the sensor really did jump that far in one frame, and the touch bit is
// lying about a finger that left. A long gap means reports went missing during
// a real movement, which is the benign kind and the reason there is no clamp
// here yet: a threshold picked without knowing which of these is happening
// would clip fast flicks on a transport that simply reports less often.
//
// The haptic tick path already distrusts jumps like this — see
// TRACKPAD_TICK_MAX_STEP in ControllerManager — but movement never learned to.
void TrackpadInput::NoteJump(const uint8_t* buf, size_t n, int dx, int dy) {
    const double dist = std::hypot(static_cast<double>(dx), static_cast<double>(dy));
    if (dist < JUMP_REPORT_UNITS) {
        m_lastFrameAt = std::chrono::steady_clock::now();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const double sinceFrameMs = m_haveFrameTime
        ? std::chrono::duration<double, std::milli>(now - m_lastFrameAt).count()
        : -1.0;
    m_lastFrameAt   = now;
    m_haveFrameTime = true;

    // One line every few seconds. A pad that jumps once jumps often, and the
    // interesting part is the shape of a handful of them, not a flood.
    if (m_haveLoggedJump
            && std::chrono::duration<double>(now - m_lastJumpLogAt).count() < JUMP_LOG_GAP_S)
        return;
    m_lastJumpLogAt  = now;
    m_haveLoggedJump = true;

    uint16_t area = 0;
    if (n >= 30) std::memcpy(&area, buf + (m_isLeftPad ? 22 : 28), 2);

    EventLog::Write("TRACKPAD: %s pad moved %.0f units in one frame "
                    "(dx=%d dy=%d, %.1fms since the last touched frame, area=%u, mode=%s)",
                    m_isLeftPad ? "left" : "right", dist, dx, dy,
                    sinceFrameMs, area, TrackpadModeId(m_mode));
}

void TrackpadInput::Update(const uint8_t* buf, size_t n, bool pressed) {
    // Only three modes read the pad's position at all. A pad set to None or
    // feeding the DS4 touchpad has nothing for this class to work out, and a
    // single button is the click bit alone, which ControllerManager reads for
    // itself alongside every other binding.
    const bool wantsMovement = m_mode == TrackpadMode::MousePointer
                            || m_mode == TrackpadMode::ScrollWheel;
    const bool wantsDirections = m_mode == TrackpadMode::DirectionalPad;
    if (n < 30 || (!wantsMovement && !wantsDirections)) return;

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

    if (wantsDirections) {
        UpdateDirections(pressed, x, y);
        return;
    }

    if (touching && m_touching) {
        const int dxRaw = static_cast<int>(x - m_prevX);
        const int dyRaw = static_cast<int>(y - m_prevY);
        NoteJump(buf, n, dxRaw, dyRaw);
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
