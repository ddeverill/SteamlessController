// Console diagnostic: drives TrackpadInput with synthesized state reports and
// checks that a directional pad resolves the direction a thumb is actually
// pressing.
//
// Worth checking rather than eyeballing, because three separate things here
// are quietly easy to get wrong and none of them fail loudly:
//
//   - Angles wrap. The RIGHT sector straddles 0/360, and a modulo on a
//     negative intermediate lands in the wrong sector rather than erroring.
//   - Hysteresis holds the current sector. Held too eagerly it never lets go
//     and the pad sticks in one direction; held too weakly it does nothing and
//     a thumb on a boundary alternates at the report rate, which — because
//     directions are edge-dispatched — is a stream of key down/up pairs.
//   - The zone is latched at the press and must never be revisited, or sliding
//     between centre and ring mid-press makes one physical click mean both a
//     direction and the click binding.
//
// Feeding whole reports rather than calling the resolver directly also checks
// the byte offsets and button bits, which is where a left-pad reader ends up
// reading right-pad coordinates.

#include "app/TrackpadInput.h"
#include "steam/SteamController.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) ++g_failures;
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
}

std::string DirsName(uint8_t d) {
    if (d == DirNone) return "none";
    std::string s;
    if (d & DirUp)    s += "Up ";
    if (d & DirDown)  s += "Down ";
    if (d & DirLeft)  s += "Left ";
    if (d & DirRight) s += "Right ";
    s.pop_back();
    return s;
}

// A state report carrying one pad's position and button state. Everything the
// resolver reads lives in the first 30 bytes; the rest stays zero.
struct Report {
    uint8_t buf[64] = {};

    Report(bool leftPad, bool touching, bool clicked, int16_t x, int16_t y) {
        buf[0] = SteamController::REPORT_STATE;
        if (leftPad) {
            if (touching) buf[5] |= SteamController::BTN_TP_LT;
            if (clicked)  buf[5] |= SteamController::BTN_TP_LT_CLICK;
            std::memcpy(buf + 18, &x, 2);
            std::memcpy(buf + 20, &y, 2);
        } else {
            if (touching) buf[4] |= SteamController::BTN_TP_RT;
            if (clicked)  buf[4] |= SteamController::BTN_TP_RT_CLICK;
            std::memcpy(buf + 24, &x, 2);
            std::memcpy(buf + 26, &y, 2);
        }
    }
};

// Feeds one frame and returns what the pad resolved.
uint8_t Feed(TrackpadInput& pad, bool leftPad, bool pressed, int16_t x, int16_t y) {
    Report r(leftPad, /*touching=*/true, pressed, x, y);
    pad.Update(r.buf, 30, pressed);
    return pad.Directions();
}

TrackpadInput MakePad(bool leftPad, DiagonalMode diagonals = DiagonalMode::EightWay) {
    TrackpadInput pad;
    pad.SetPad(leftPad);
    pad.SetMode(TrackpadMode::DirectionalPad);
    pad.SetDiagonals(diagonals);
    return pad;
}

// A point at the given bearing, far enough out to be a ring press.
void AtAngle(double deg, int radius, int16_t& x, int16_t& y) {
    const double rad = deg * 3.14159265358979323846 / 180.0;
    x = static_cast<int16_t>(std::cos(rad) * radius);
    y = static_cast<int16_t>(std::sin(rad) * radius);
}

void CheckAngle(bool leftPad, double deg, uint8_t want) {
    TrackpadInput pad = MakePad(leftPad);
    int16_t x = 0, y = 0;
    AtAngle(deg, 25000, x, y);
    const uint8_t got = Feed(pad, leftPad, /*pressed=*/true, x, y);
    char label[96];
    snprintf(label, sizeof(label), "%5.1f deg -> %-16s (wanted %s)",
             deg, DirsName(got).c_str(), DirsName(want).c_str());
    Check(got == want, label);
}

}  // namespace

int main() {
    printf("Eight-way sectors, right pad\n");
    CheckAngle(false,   0.0, DirRight);
    CheckAngle(false,  45.0, DirUp   | DirRight);
    CheckAngle(false,  90.0, DirUp);
    CheckAngle(false, 135.0, DirUp   | DirLeft);
    CheckAngle(false, 180.0, DirLeft);
    CheckAngle(false, 225.0, DirDown | DirLeft);
    CheckAngle(false, 270.0, DirDown);
    CheckAngle(false, 315.0, DirDown | DirRight);
    // The RIGHT sector straddles the wrap, so both sides of it have to land in
    // the same place.
    CheckAngle(false,  20.0, DirRight);
    CheckAngle(false, 340.0, DirRight);
    CheckAngle(false, 359.9, DirRight);

    printf("\nThe left pad reads its own coordinates\n");
    CheckAngle(true,  90.0, DirUp);
    CheckAngle(true, 180.0, DirLeft);
    {
        // A report carrying only right-pad data must leave a left pad idle —
        // this is the check that a swapped offset would fail.
        TrackpadInput pad = MakePad(true);
        int16_t x = 0, y = 0;
        AtAngle(90.0, 25000, x, y);
        Report r(/*leftPad=*/false, true, true, x, y);
        pad.Update(r.buf, 30, /*pressed=*/true);
        Check(pad.Directions() == DirNone, "left pad ignores a right-pad press");
    }

    printf("\nFour-way rounds diagonals to one direction\n");
    {
        // The boundary sits at 45 degrees, so a press either side of it rounds
        // to the nearer cardinal and never presses two at once.
        TrackpadInput pad = MakePad(false, DiagonalMode::FourWay);
        int16_t x = 0, y = 0;
        AtAngle(40.0, 25000, x, y);
        Check(Feed(pad, false, true, x, y) == DirRight, "40 deg rounds to Right alone");
    }
    {
        TrackpadInput pad = MakePad(false, DiagonalMode::FourWay);
        int16_t x = 0, y = 0;
        AtAngle(50.0, 25000, x, y);
        Check(Feed(pad, false, true, x, y) == DirUp, "50 deg rounds to Up alone");
    }
    {
        // The same bearing that is a diagonal in eight-way.
        TrackpadInput eight = MakePad(false);
        TrackpadInput four  = MakePad(false, DiagonalMode::FourWay);
        int16_t x = 0, y = 0;
        AtAngle(46.0, 25000, x, y);
        Check(Feed(eight, false, true, x, y) == (DirUp | DirRight),
              "46 deg is Up+Right in eight-way");
        Check(Feed(four, false, true, x, y) == DirUp,
              "46 deg is Up alone in four-way");
    }

    printf("\nThe centre is the click binding, not a direction\n");
    {
        TrackpadInput pad = MakePad(false);
        Check(Feed(pad, false, true, 0, 0) == DirNone, "dead centre presses nothing");
        Check(pad.ClickInCentre(), "and reports itself as a centre click");
    }
    {
        TrackpadInput pad = MakePad(false);
        int16_t x = 0, y = 0;
        AtAngle(90.0, kPadRingRadius - 500, x, y);
        Check(Feed(pad, false, true, x, y) == DirNone, "just inside the ring is still centre");
    }
    {
        TrackpadInput pad = MakePad(false);
        int16_t x = 0, y = 0;
        AtAngle(90.0, kPadRingRadius + 500, x, y);
        Check(Feed(pad, false, true, x, y) == DirUp, "just outside the ring is a direction");
        Check(!pad.ClickInCentre(), "and reports itself as a ring click");
    }

    printf("\nTouch alone presses nothing — directions come from the click\n");
    {
        TrackpadInput pad = MakePad(false);
        int16_t x = 0, y = 0;
        AtAngle(90.0, 25000, x, y);
        Check(Feed(pad, false, /*pressed=*/false, x, y) == DirNone,
              "a thumb resting out in the ring presses nothing");
    }

    printf("\nThe zone latches at the press and is never revisited\n");
    {
        // Press in the ring, then slide into the centre while still held. The
        // direction has to survive: converting to a centre click here would
        // make one physical press mean two things.
        TrackpadInput pad = MakePad(false);
        int16_t x = 0, y = 0;
        AtAngle(90.0, 25000, x, y);
        Check(Feed(pad, false, true, x, y) == DirUp, "ring press gives Up");
        Check(Feed(pad, false, true, 0, 0) == DirUp, "sliding to dead centre keeps Up");
        Check(!pad.ClickInCentre(), "and it is still a ring click");
    }
    {
        // The mirror: press in the centre, slide out to the rim while held.
        TrackpadInput pad = MakePad(false);
        Check(Feed(pad, false, true, 0, 0) == DirNone, "centre press gives nothing");
        int16_t x = 0, y = 0;
        AtAngle(90.0, 30000, x, y);
        Check(Feed(pad, false, true, x, y) == DirNone, "sliding to the rim still gives nothing");
        Check(pad.ClickInCentre(), "and it is still a centre click");
    }

    printf("\nReleasing clears everything\n");
    {
        TrackpadInput pad = MakePad(false);
        int16_t x = 0, y = 0;
        AtAngle(90.0, 25000, x, y);
        Feed(pad, false, true, x, y);
        Check(Feed(pad, false, /*pressed=*/false, x, y) == DirNone, "release drops the direction");
        // A fresh press re-decides the zone, so the pad is usable again.
        Check(Feed(pad, false, true, 0, 0) == DirNone, "next press re-reads the zone as centre");
        Check(pad.ClickInCentre(), "and says so");
    }

    printf("\nHysteresis: a thumb on a boundary does not alternate\n");
    {
        // The Up/Up-Right boundary sits at 67.5 degrees. Hold Up, then creep
        // across it — the direction must not change until the thumb has
        // cleared the boundary by the margin.
        TrackpadInput pad = MakePad(false);
        int16_t x = 0, y = 0;
        AtAngle(90.0, 25000, x, y);
        Check(Feed(pad, false, true, x, y) == DirUp, "starts on Up");

        AtAngle(66.0, 25000, x, y);
        Check(Feed(pad, false, true, x, y) == DirUp,
              "1.5 deg past the boundary still Up");
        AtAngle(60.0, 25000, x, y);
        Check(Feed(pad, false, true, x, y) == DirUp,
              "7.5 deg past the boundary still Up");
        AtAngle(56.0, 25000, x, y);
        Check(Feed(pad, false, true, x, y) == (DirUp | DirRight),
              "11.5 deg past the boundary finally switches");
        // Having switched, it must be just as reluctant to switch back, or the
        // thumb sitting where it is would flip on the next frame.
        AtAngle(60.0, 25000, x, y);
        Check(Feed(pad, false, true, x, y) == (DirUp | DirRight),
              "and does not immediately switch back");
    }
    {
        // Hysteresis must not become a trap: a deliberate move to the opposite
        // side of the pad has to register.
        TrackpadInput pad = MakePad(false);
        int16_t x = 0, y = 0;
        AtAngle(90.0, 25000, x, y);
        Feed(pad, false, true, x, y);
        AtAngle(270.0, 25000, x, y);
        Check(Feed(pad, false, true, x, y) == DirDown, "Up to Down still registers");
    }

    printf("\nOther modes resolve no directions at all\n");
    for (auto mode : { TrackpadMode::None, TrackpadMode::MousePointer,
                       TrackpadMode::ScrollWheel, TrackpadMode::DS4Touchpad,
                       TrackpadMode::SingleButton }) {
        TrackpadInput pad;
        pad.SetPad(false);
        pad.SetMode(mode);
        int16_t x = 0, y = 0;
        AtAngle(90.0, 25000, x, y);
        Report r(false, true, true, x, y);
        pad.Update(r.buf, 30, /*pressed=*/true);
        char label[80];
        snprintf(label, sizeof(label), "mode %-8s presses no direction", TrackpadModeId(mode));
        Check(pad.Directions() == DirNone, label);
        // Every non-directional pad has to answer "centre", so callers can gate
        // the click binding on it without asking what mode they are in.
        snprintf(label, sizeof(label), "mode %-8s reports a centre click", TrackpadModeId(mode));
        Check(pad.ClickInCentre(), label);
    }

    printf("\nA short report is ignored rather than read past\n");
    {
        TrackpadInput pad = MakePad(false);
        int16_t x = 0, y = 0;
        AtAngle(90.0, 25000, x, y);
        Report r(false, true, true, x, y);
        pad.Update(r.buf, 20, /*pressed=*/true);
        Check(pad.Directions() == DirNone, "29 bytes or fewer resolves nothing");
    }

    printf("\n%s (%d failure(s))\n", g_failures ? "FAILED" : "All checks passed", g_failures);
    return g_failures ? 1 : 0;
}
