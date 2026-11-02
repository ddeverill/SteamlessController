// Console diagnostic for the stick deadzone (#117): that rest reads as nothing,
// that the edge of the deadzone is continuous rather than a step, that full
// deflection still reaches full, and that a direction is left alone. None of it
// needs a controller.
//
// The size of the deadzone comes from StickRestProbe's measurements of real
// hardware; this checks that the maths honours whatever size it is given, which
// an off-by-one at the edge or a lost corner would get quietly wrong.
#include "app/StickConfig.h"
#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) ++g_failures;
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
}

double Len(StickPos p) {
    return std::hypot(static_cast<double>(p.x), static_cast<double>(p.y));
}

}  // namespace

int main() {
    const int dz = kStickDeadzoneRaw;

    printf("Inside the deadzone\n");
    {
        const StickPos zero = ApplyStickDeadzone(0, 0);
        Check(zero.x == 0 && zero.y == 0, "exact centre is nothing");

        const int16_t small = static_cast<int16_t>(dz / 2);
        const StickPos a = ApplyStickDeadzone(small, small);
        Check(a.x == 0 && a.y == 0, "a small diagonal drift is nothing");
        const StickPos b = ApplyStickDeadzone(static_cast<int16_t>(-small), small);
        Check(b.x == 0 && b.y == 0, "and so is one leaning the other way");

        // Radial, not per-axis: this point has each axis inside the deadzone but
        // its length well outside it, so a per-axis deadzone would zero it.
        const int16_t each = static_cast<int16_t>(dz * 0.9);
        const StickPos c = ApplyStickDeadzone(each, each);
        Check(c.x != 0 && c.y != 0, "a diagonal outside the circle is not cut per axis");

        const StickPos at = ApplyStickDeadzone(static_cast<int16_t>(dz), 0);
        Check(at.x == 0 && at.y == 0, "exactly on the edge is still nothing");
    }

    printf("\nWhat real sticks rest at\n");
    {
        // From StickRestProbe, hands off. The left stick's measured X and Y
        // ranges reach -587 and 1865, so their corner is a position harsher
        // than any single report it produced (the largest was radius 1923).
        const StickPos worst = ApplyStickDeadzone(-587, 1865);   // radius 1955, 5.97%
        Check(worst.x == 0 && worst.y == 0, "the worst measured resting position reads as nothing");
        const StickPos right = ApplyStickDeadzone(658, 111);     // radius 667
        Check(right.x == 0 && right.y == 0, "so does the other stick's");
        Check(Len(StickPos{ -587, 1865 }) < dz, "with room to spare, not by a hair");
        Check(dz >= static_cast<int>(1.3 * 1955), "at least 30% more than the worst rest seen");
    }

    printf("\nJust outside it\n");
    {
        const StickPos p = ApplyStickDeadzone(static_cast<int16_t>(dz + 1), 0);
        Check(p.x >= 0 && p.x <= 5, "one past the edge starts from near zero, not a step");
        Check(Len(ApplyStickDeadzone(static_cast<int16_t>(dz + 400), 0))
                  > Len(ApplyStickDeadzone(static_cast<int16_t>(dz + 200), 0)),
              "and grows with the stick");
    }

    printf("\nFull deflection\n");
    {
        const StickPos right = ApplyStickDeadzone(32767, 0);
        Check(right.x == 32767 && right.y == 0, "full right is still full right");
        const StickPos left = ApplyStickDeadzone(-32768, 0);
        Check(left.x == -32768 && left.y == 0, "full left reaches the bottom of the range");
        const StickPos up = ApplyStickDeadzone(0, 32767);
        Check(up.x == 0 && up.y == 32767, "full up is still full up");
        const StickPos down = ApplyStickDeadzone(0, -32768);
        Check(down.x == 0 && down.y == -32768, "full down reaches the bottom of the range");

        // A square gate's corner has a length past a circle's edge. It must not
        // come out smaller than it went in.
        const StickPos corner = ApplyStickDeadzone(32767, 32767);
        Check(corner.x == 32767 && corner.y == 32767, "a pushed-in corner still reaches full on both axes");
        const StickPos negCorner = ApplyStickDeadzone(-32768, -32768);
        Check(negCorner.x == -32768 && negCorner.y == -32768, "and so does the opposite corner");
    }

    printf("\nDirection\n");
    {
        // 30 degrees, at 60% of full. The angle must survive the rescale.
        const double mag = 0.6 * kStickRawMax;
        const double ang = 30.0 * 3.14159265358979323846 / 180.0;
        const StickPos p = ApplyStickDeadzone(static_cast<int16_t>(mag * std::cos(ang)),
                                              static_cast<int16_t>(mag * std::sin(ang)));
        const double outAng = std::atan2(static_cast<double>(p.y), static_cast<double>(p.x))
                            * 180.0 / 3.14159265358979323846;
        Check(std::fabs(outAng - 30.0) < 0.1, "a 30 degree push comes out at 30 degrees");
        Check(Len(p) < mag, "and comes out smaller than it went in, as the deadzone eats the bottom");
    }

    printf("\nSymmetry\n");
    {
        const StickPos a = ApplyStickDeadzone(12000, -5000);
        const StickPos b = ApplyStickDeadzone(-12000, 5000);
        Check(a.x == -b.x && a.y == -b.y, "opposite pushes come out opposite");
    }

    printf("\nOddly sized deadzones\n");
    {
        const StickPos off = ApplyStickDeadzone(500, 500, 0);
        Check(off.x == 500 && off.y == 500, "a zero deadzone passes the stick straight through");
        const StickPos neg = ApplyStickDeadzone(500, 500, -10);
        Check(neg.x == 500 && neg.y == 500, "a negative one does too");
        const StickPos huge = ApplyStickDeadzone(32767, 0, 100000);
        Check(huge.x == 32767 && huge.y == 0, "one wider than the range still reaches full travel");
    }

    // Stick settings (#131): Default is the 8% circle untouched, Circle at 8 and
    // 100 is the same thing, Square is the same deadzone read one axis at a time,
    // and the outer point is where full output starts.
    printf("\nStick settings\n");
    {
        const StickSettings dflt;
        const StickSettings circle{ StickDeadzoneMode::Circle, 8, 100 };
        const StickSettings square{ StickDeadzoneMode::Square, 8, 100 };

        bool same = true;
        const int16_t samples[] = { -32768, -20000, -9000, -2600, -300, 0, 400, 2621, 2622, 5000, 17000, 32767 };
        for (int16_t sx : samples)
            for (int16_t sy : samples) {
                const StickPos a = ApplyStickDeadzone(sx, sy);
                const StickPos b = ApplyStick(sx, sy, dflt);
                const StickPos c = ApplyStick(sx, sy, circle);
                if (a.x != b.x || a.y != b.y || a.x != c.x || a.y != c.y) same = false;
            }
        Check(same, "Default, and Circle at 8 and 100, read every sample exactly as the old deadzone did");

        const StickPos axisOnly = ApplyStick(20000, 1500, square);
        Check(axisOnly.y == 0 && axisOnly.x > 0,
              "square: a push along one axis with a little on the other leaves the other at nothing");
        const StickPos circleSame = ApplyStick(20000, 1500, circle);
        Check(circleSame.y != 0, "which circle, measuring from the centre, does not");

        const StickPos diag = ApplyStick(32767, 32767, square);
        Check(diag.x == 32767 && diag.y == 32767, "square: a diagonal reaches full on both axes");
        const StickPos diagNeg = ApplyStick(-32768, -32768, square);
        Check(diagNeg.x == -32768 && diagNeg.y == -32768, "and so does the opposite one, to the bottom of the range");
        const StickPos edge = ApplyStick(static_cast<int16_t>(kStickDeadzoneRaw), 0, square);
        Check(edge.x == 0, "square: the edge of the deadzone is nothing");
        const StickPos past = ApplyStick(static_cast<int16_t>(kStickDeadzoneRaw + 1), 0, square);
        Check(past.x > 0 && past.x < 100, "and one step past it is a hair of input, not a jump");

        const StickSettings early{ StickDeadzoneMode::Circle, 3, 76 };
        const int16_t at76 = static_cast<int16_t>(kStickRawMax * 76 / 100);
        const StickPos full = ApplyStick(at76, 0, early);
        Check(std::abs(full.x - kStickRawMax) <= 2, "circle: pushed to the outer point reads full");
        const StickPos beyond = ApplyStick(32767, 0, early);
        Check(beyond.x == 32767, "and so does anything past it");
        const StickPos below = ApplyStick(static_cast<int16_t>(kStickRawMax * 3 / 100), 0, early);
        Check(below.x == 0, "circle: at the inner point it is nothing");
        const StickPos mid = ApplyStick(static_cast<int16_t>(kStickRawMax * 40 / 100), 0, early);
        Check(mid.x > 0 && mid.x < kStickRawMax, "and between the two it is on the ramp");

        const StickSettings sqEarly{ StickDeadzoneMode::Square, 3, 76 };
        const StickPos sqFull = ApplyStick(at76, at76, sqEarly);
        Check(std::abs(sqFull.x - kStickRawMax) <= 2 && std::abs(sqFull.y - kStickRawMax) <= 2,
              "square: pushed to the outer point on both axes reads full on both");

        const StickSettings none{ StickDeadzoneMode::Circle, 0, 100 };
        const StickPos through = ApplyStick(500, -700, none);
        Check(through.x == 500 && through.y == -700, "an inner point of 0 with the outer at 100 passes the stick straight through");

        const StickSettings wild{ StickDeadzoneMode::Circle, 99, 3 };
        const StickPos tame = ApplyStick(32767, 0, wild);
        Check(tame.x == 32767, "points that are out of order or too close are pulled into a usable range");

        uint32_t inner = 99, outer = 3;
        ClampStickRange(inner, outer);
        Check(inner == 95 && outer == 100, "the clamp holds the inner point and moves the outer one");
    }

    printf("\n%s\n", g_failures == 0 ? "All checks passed." : "FAILURES.");
    return g_failures == 0 ? 0 : 1;
}
