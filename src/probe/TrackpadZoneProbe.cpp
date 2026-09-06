// TrackpadZoneProbe — measures where a thumb actually lands when it clicks a
// trackpad, so the Directional Pad ring boundary can be a number rather than a
// guess.
//
// Directional Pad mode splits a pad by location: a click near the centre is the
// Trackpad Click binding, a click out in the ring is the direction under the
// thumb. That split needs three numbers this tool measures and nothing else in
// the tree reports:
//
//   1. The usable radius. "The pads span roughly +/-32000" is a comment in
//      TrackpadInput.h, not a measurement, and the radius a thumb can actually
//      reach is smaller than the one the sensor can encode.
//   2. Where deliberate centre presses land versus deliberate direction
//      presses, and whether the two are separable by radius at all.
//   3. How far the reported centroid moves between the thumb touching down and
//      the click bit going high. Pressing a pad flattens the fingertip and
//      tilts the surface, so the position at the click frame is not where the
//      user aimed — ControllerManager's haptic code already carries
//      kPressConfirmFrames and TRACKPAD_TICK_MAX_STEP for the same reason.
//      This decides how many frames back the zone should be sampled from.
//
// Non-invasive by construction: opens the device shared, only ever reads, and
// never sends a report or touches lizard mode. Steam can keep running.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "hid/HidDevice.h"
#include "steam/SteamController.h"
// For kPadRingRadius, so this tool and the code it is checking cannot disagree
// about where the centre ends.
#include "app/TrackpadConfig.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

// How many frames of history to keep per pad. The deepest lookback the summary
// reports is 8; the rest is slack so a wrapped read cannot outrun it.
constexpr size_t kHistoryFrames  = 16;
constexpr size_t kMaxLookback    = 8;

// ---------------------------------------------------------------------------
// Direction naming
// ---------------------------------------------------------------------------

// Eight-way sectors, counter-clockwise from east. Pad Y grows upward, so 90
// degrees is up — the same convention TrackpadInput will resolve against.
const char* Sector8Name(int s) {
    static const char* names[] = { "RIGHT", "UP-RIGHT", "UP", "UP-LEFT",
                                   "LEFT", "DOWN-LEFT", "DOWN", "DOWN-RIGHT" };
    return (s >= 0 && s < 8) ? names[s] : "CENTRE";
}

const char* Sector4Name(int s) {
    static const char* names[] = { "RIGHT", "UP", "LEFT", "DOWN" };
    return (s >= 0 && s < 4) ? names[s] : "CENTRE";
}

// Angle in [0, 360), measured counter-clockwise from east.
double AngleDeg(int16_t x, int16_t y) {
    double a = std::atan2(static_cast<double>(y), static_cast<double>(x)) * 180.0 / kPi;
    if (a < 0.0) a += 360.0;
    return a;
}

int Sector8(double angleDeg) {
    return static_cast<int>(std::floor((angleDeg + 22.5) / 45.0)) % 8;
}

int Sector4(double angleDeg) {
    return static_cast<int>(std::floor((angleDeg + 45.0) / 90.0)) % 4;
}

double Radius(int16_t x, int16_t y) {
    return std::hypot(static_cast<double>(x), static_cast<double>(y));
}

// ---------------------------------------------------------------------------
// Per-pad frame history
// ---------------------------------------------------------------------------

struct PadHistory {
    struct Frame {
        int16_t  x = 0;
        int16_t  y = 0;
        uint16_t area = 0;
        bool     touching = false;
    };

    Frame  frames[kHistoryFrames]{};
    size_t next = 0;

    void Push(const Frame& f) {
        frames[next] = f;
        next = (next + 1) % kHistoryFrames;
    }

    // k frames back from the most recent push; k = 0 is the most recent.
    const Frame& Back(size_t k) const {
        const size_t i = (next + kHistoryFrames - 1 - (k % kHistoryFrames)) % kHistoryFrames;
        return frames[i];
    }
};

// ---------------------------------------------------------------------------
// Calibration stages
// ---------------------------------------------------------------------------

struct Stage {
    const char* prompt;
    int         wantSector8;  // -1 for the centre stage
    int         reps;
};

// Six centre presses because that distribution's upper tail is what sets the
// boundary; two per diagonal because diagonals only need to confirm the sector
// math, not pin down a radius.
const Stage kStages[] = {
    { "the CENTRE of the pad", -1, 6 },
    { "UP",                     2, 3 },
    { "RIGHT",                  0, 3 },
    { "DOWN",                   6, 3 },
    { "LEFT",                   4, 3 },
    { "UP-RIGHT",               1, 2 },
    { "DOWN-RIGHT",             7, 2 },
    { "DOWN-LEFT",              5, 2 },
    { "UP-LEFT",                3, 2 },
};
constexpr int kStageCount = static_cast<int>(sizeof(kStages) / sizeof(kStages[0]));

// One recorded click.
struct Press {
    int      stage = -1;        // index into kStages, or -1 in free mode
    double   r = 0.0;
    double   angle = 0.0;
    int16_t  x = 0;
    int16_t  y = 0;
    uint16_t areaAtClick = 0;
    uint16_t areaPre = 0;
    // Distance between the click frame's centroid and the one k frames earlier,
    // for k = 1..kMaxLookback. Negative where the pad was not being touched
    // that far back — a fast stab has no history to look back into.
    double   drift[kMaxLookback + 1]{};
};

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

std::mutex         g_mutex;
std::vector<Press> g_presses;
double             g_maxTouchRadius = 0.0;  // any touched frame, either pad
std::atomic<bool>  g_quit{false};
std::atomic<bool>  g_useLeftPad{false};
// -1 when not calibrating; otherwise the stage being collected.
std::atomic<int>   g_stage{-1};
std::atomic<int>   g_stageDone{0};

void PrintStagePrompt(int stage) {
    if (stage < 0 || stage >= kStageCount) return;
    printf("\n[%d/%d] Click %s  (%d time%s)\n> ",
           stage + 1, kStageCount, kStages[stage].prompt,
           kStages[stage].reps, kStages[stage].reps == 1 ? "" : "s");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double pos = p * static_cast<double>(v.size() - 1);
    const size_t i = static_cast<size_t>(std::lround(pos));
    return v[std::min(i, v.size() - 1)];
}

void PrintSummary() {
    std::lock_guard<std::mutex> lk(g_mutex);

    if (g_presses.empty()) {
        puts("\nNo presses recorded yet.");
        return;
    }

    std::vector<double> centreR, directionR;
    for (const auto& p : g_presses) {
        if (p.stage < 0) continue;  // free-mode presses are unlabelled
        if (kStages[p.stage].wantSector8 < 0) centreR.push_back(p.r);
        else                                  directionR.push_back(p.r);
    }

    printf("\n===========================================================\n");
    printf("  Trackpad zone calibration — %s pad\n",
           g_useLeftPad.load() ? "LEFT" : "RIGHT");
    printf("===========================================================\n\n");

    printf("Usable radius (max over every touched frame): %.0f\n\n", g_maxTouchRadius);

    if (!centreR.empty()) {
        printf("CENTRE presses (n=%zu):\n", centreR.size());
        printf("  r   min %.0f   p50 %.0f   p95 %.0f   max %.0f\n\n",
               Percentile(centreR, 0.0), Percentile(centreR, 0.50),
               Percentile(centreR, 0.95), Percentile(centreR, 1.0));
    }
    if (!directionR.empty()) {
        printf("DIRECTION presses (n=%zu):\n", directionR.size());
        printf("  r   min %.0f   p05 %.0f   p50 %.0f   max %.0f\n\n",
               Percentile(directionR, 0.0), Percentile(directionR, 0.05),
               Percentile(directionR, 0.50), Percentile(directionR, 1.0));
    }

    // The boundary question: is a radius threshold able to tell the two kinds
    // of press apart at all, and if so where does it sit?
    if (!centreR.empty() && !directionR.empty()) {
        const double centreHigh = Percentile(centreR, 0.95);
        const double dirLow     = Percentile(directionR, 0.05);
        printf("SEPARATION\n");
        printf("  centre p95     %.0f\n", centreHigh);
        printf("  direction p05  %.0f\n", dirLow);
        if (dirLow > centreHigh) {
            const double boundary = (centreHigh + dirLow) / 2.0;
            const double frac = g_maxTouchRadius > 0.0 ? boundary / g_maxTouchRadius : 0.0;
            printf("  gap            %.0f units — separable\n\n", dirLow - centreHigh);
            printf("  RECOMMENDED RING BOUNDARY: %.0f  (%.2f of usable radius)\n\n",
                   boundary, frac);
        } else {
            printf("  gap            NONE — the distributions overlap\n\n");
            printf("  A radius threshold cannot separate these presses at this\n"
                   "  grip. Either the centre zone needs to be smaller than the\n"
                   "  presses being aimed at it, or zoning by radius is the wrong\n"
                   "  split for this hand position.\n\n");
        }
    }

    // Does the sector maths agree with what the user was asked to press? A low
    // hit rate on the diagonals, or a wide angular spread, is what argues for
    // how much hysteresis the boundaries need.
    bool anyLabelled = false;
    for (int s = 0; s < kStageCount; ++s) {
        if (kStages[s].wantSector8 < 0) continue;
        int hits = 0, total = 0;
        double sumSin = 0.0, sumCos = 0.0;
        for (const auto& p : g_presses) {
            if (p.stage != s) continue;
            ++total;
            if (Sector8(p.angle) == kStages[s].wantSector8) ++hits;
            const double rad = p.angle * kPi / 180.0;
            sumSin += std::sin(rad);
            sumCos += std::cos(rad);
        }
        if (total == 0) continue;
        if (!anyLabelled) {
            printf("SECTOR CLASSIFICATION (eight-way)\n");
            anyLabelled = true;
        }
        double mean = std::atan2(sumSin, sumCos) * 180.0 / kPi;
        if (mean < 0.0) mean += 360.0;
        printf("  %-11s %d/%d correct   mean angle %6.1f deg   (sector centre %5.1f)\n",
               Sector8Name(kStages[s].wantSector8), hits, total, mean,
               static_cast<double>(kStages[s].wantSector8) * 45.0);
    }
    if (anyLabelled) printf("\n");

    // Centroid drift, which sets how many frames back the zone should be
    // sampled from at the click edge.
    printf("CENTROID DRIFT (touch-down toward click, mean over %zu presses)\n",
           g_presses.size());
    for (size_t k = 1; k <= kMaxLookback; ++k) {
        double sum = 0.0, worst = 0.0;
        int n = 0;
        for (const auto& p : g_presses) {
            if (p.drift[k] < 0.0) continue;  // no touched history that far back
            sum += p.drift[k];
            worst = std::max(worst, p.drift[k]);
            ++n;
        }
        if (n == 0) {
            printf("  lookback %zu frame%s : (no samples)\n", k, k == 1 ? " " : "s");
            continue;
        }
        printf("  lookback %zu frame%s : mean %6.0f   max %6.0f   (n=%d)\n",
               k, k == 1 ? " " : "s", sum / n, worst, n);
    }

    double areaPre = 0.0, areaClick = 0.0;
    int areaN = 0;
    for (const auto& p : g_presses) {
        if (p.areaPre == 0 && p.areaAtClick == 0) continue;
        areaPre   += p.areaPre;
        areaClick += p.areaAtClick;
        ++areaN;
    }
    if (areaN > 0) {
        printf("\nCONTACT AREA   pre-press mean %.0f  ->  at-click mean %.0f\n",
               areaPre / areaN, areaClick / areaN);
    }
    printf("\n");
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Tap tracking — why a haptic does not fire on every press
// ---------------------------------------------------------------------------

// The live constants from ControllerManager's click haptic latch, mirrored so
// the session can be replayed through the real state machine rather than
// through a description of it. Keep in step with Slot.
constexpr int kPressConfirmFrames   = 2;
constexpr int kReleaseConfirmFrames = 4;

struct Tap {
    uint16_t areaAtPress = 0;
    // Frames between the finger landing and the click bit going high. A press
    // sets the touch bit on its way down, so a touch binding fired off the raw
    // bit fires on every press — this is the window a tap has to outlast
    // before it counts as one, and the shortest of these is the ceiling on
    // ControllerManager's kTouchConfirmFrames. -1 when the finger was already
    // down before this session started.
    int      touchToClick = -1;
    // Where the press landed, and therefore what a directional pad made of it.
    // A press inside the ring is the pad's click binding rather than a
    // direction, which to a thumb aiming for a direction is indistinguishable
    // from the press not registering at all.
    double   radius = 0.0;
    double   angle  = 0.0;
    bool     centre = false;
    // Frames the click bit stayed high. The virtual pad is updated every frame
    // at ~250Hz, but a game reads it at its own rate — 60Hz is one poll every
    // four frames — so a press shorter than that can be dispatched perfectly,
    // fire its haptic, and still fall between two polls unseen.
    int      highFrames = 0;
    // Frames the click bit stayed low after this press before going high
    // again, and the lowest contact area reached in that gap. Together these
    // say whether a release is distinguishable from threshold chatter, and
    // whether the thumb ever came near the idle area the latch waits for.
    int      lowFrames   = 0;
    uint16_t minGapArea  = 0xFFFF;
    bool     gapClosed   = false;  // the next press arrived, so the gap is final
    bool     haptic      = false;  // the latch would have fired for this press
};

struct TapTracker {
    std::vector<Tap> taps;
    bool prevClick = false;

    // Frames actually seen, and over how long. A session that captures fewer
    // presses than the thumb made is worthless until this says why: at the
    // report rate the stream is healthy and the presses genuinely were not
    // there, and far below it the reports are being dropped before this sees
    // them. Two processes reading one device each get their own queue, and an
    // overflowing queue drops silently.
    long long frames = 0;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool haveStart = false;

    // A replay of the real latch, frame for frame.
    enum class State { WaitingForPress, WaitingForRelease };
    State state         = State::WaitingForPress;
    int   clickTrue     = 0;
    int   clickLow      = 0;
    int   hapticsFired  = 0;   // presses and releases both pulse

    // Frames the finger has been down for, or -1 when it is not down. Starts
    // at -1 so a finger already resting when the session opens is not credited
    // with a landing nobody saw.
    int  framesDown = -1;
    bool prevTouch  = false;

    // Presses counted from contact area instead of the firmware's click bit.
    // A press flattens the fingertip, so it shows up in the area whether or
    // not the firmware calls it a click — if a thumb makes twenty presses and
    // the click bit rises four times, this says whether the other sixteen are
    // in the report at all. Three thresholds because the right one is exactly
    // what is unknown; the count that matches the presses actually made is the
    // answer. Hysteresis on the way down so one press counts once.
    struct Spikes {
        int  count = 0;
        bool high  = false;
        void Frame(uint16_t area, uint16_t up, uint16_t down) {
            if (!high && area >= up)        { high = true; ++count; }
            else if (high && area < down)   { high = false; }
        }
    };
    Spikes spike3000, spike3400, spike3800;
    uint16_t maxArea = 0;

    void Frame(bool touching, bool click, uint16_t area, int16_t x, int16_t y) {
        if (!haveStart) { started = std::chrono::steady_clock::now(); haveStart = true; }
        ++frames;

        if (!touching)            framesDown = -1;
        else if (!prevTouch)      framesDown = 0;
        else if (framesDown >= 0) ++framesDown;
        prevTouch = touching;

        spike3000.Frame(area, 3000, 2400);
        spike3400.Frame(area, 3400, 2800);
        spike3800.Frame(area, 3800, 3200);
        if (area > maxArea) maxArea = area;

        // --- what the user did ---
        if (click && !prevClick) {
            if (!taps.empty() && !taps.back().gapClosed) taps.back().gapClosed = true;
            // Echoed as it happens, so a session that is missing presses says
            // so while there is still a thumb on the pad to check it with,
            // rather than at the summary once the evidence is gone.
            Tap t;
            t.areaAtPress  = area;
            t.touchToClick = framesDown;
            t.radius       = Radius(x, y);
            t.angle        = AngleDeg(x, y);
            t.centre       = t.radius < kPadRingRadius;
            printf("  press %2zu  r %6.0f  %-9s %-10s  area %5u\n",
                   taps.size() + 1, t.radius,
                   t.centre ? "CENTRE" : "ring",
                   t.centre ? "(click)" : Sector8Name(Sector8(t.angle)),
                   area);
            fflush(stdout);
            taps.push_back(t);
        }
        if (click && !taps.empty()) ++taps.back().highFrames;
        if (!click && !taps.empty() && !taps.back().gapClosed) {
            Tap& t = taps.back();
            ++t.lowFrames;
            t.minGapArea = std::min(t.minGapArea, area);
        }

        // --- what the latch would have done ---
        clickTrue = click ? clickTrue + 1 : 0;
        clickLow  = click ? 0 : clickLow + 1;
        if (state == State::WaitingForPress) {
            if (clickTrue >= kPressConfirmFrames) {
                state = State::WaitingForRelease;
                ++hapticsFired;
                if (!taps.empty()) taps.back().haptic = true;
            }
        } else if (clickLow >= kReleaseConfirmFrames) {
            state = State::WaitingForPress;
            ++hapticsFired;
        }

        prevClick = click;
    }
};

std::mutex g_tapMutex;
TapTracker g_tap;
std::atomic<bool> g_tapMode{false};

// ---------------------------------------------------------------------------
// Press calibration — where "resting" ends and "pressing" begins
// ---------------------------------------------------------------------------
//
// The firmware's click bit misses roughly half of a thumb's presses when the
// thumb stays on the pad, but the presses are in the report: contact area
// spikes for every one of them, because a press flattens the fingertip. So a
// press can be detected from area instead — which needs one number, the area
// above which a thumb is pressing rather than resting.
//
// Both halves of that are measured rather than assumed, in two stages: rest
// without pressing, then press a known number of times. A threshold is then
// swept across the gap and the one that recovers exactly the presses made is
// the answer. Guessing it is how you get a pad that fires a direction because
// a thumb is lying on it.
enum class PressCal { Off = 0, Resting = 1, Pressing = 2 };
std::atomic<int> g_pressCal{static_cast<int>(PressCal::Off)};
std::mutex g_calMutex;
std::vector<uint16_t> g_restAreas;   // touched-but-not-pressing samples
std::vector<uint16_t> g_pressAreas;  // the whole series, 0 while untouched
int g_calExpectedPresses = 15;

// Five seconds of contact is plenty to characterise a resting thumb, and short
// enough that nobody has to hold still for long.
constexpr size_t kRestSamples = 1250;

// What the reader did with every report that arrived, so a session can prove
// it is not the one losing presses.
std::atomic<long long> g_reportsSeen{0};
std::atomic<long long> g_reportsWrongId{0};
std::atomic<long long> g_reportsShort{0};
std::atomic<long long> g_clicksDiscarded{0};
std::atomic<long long> g_readTimeouts{0};
std::atomic<int>       g_lastWrongId{-1};

// Counts presses in an area series against ONE threshold: above it is pressed,
// below it is not. No second "backed off" level, which would be a per-person
// number with nothing behind it — thumbs differ enormously in how heavily they
// rest, and a release level guessed too high latches the detector and swallows
// every press after the first.
//
// `confirmFrames` is how many consecutive frames a side must hold before the
// state changes. Not a per-person number: it describes the sensor's noise, not
// anyone's grip. Without it a single threshold chatters, because the area
// passes through the line on the way up and again on the way down, and a few
// counts of noise while it sits there reads as several presses.
int CountCrossings(const std::vector<uint16_t>& series, int threshold,
                   int confirmFrames) {
    int  count = 0, above = 0, below = 0;
    bool high = false;
    for (uint16_t a : series) {
        if (a >= threshold) { ++above; below = 0; }
        else                { ++below; above = 0; }
        if (!high && above >= confirmFrames)      { high = true;  ++count; }
        else if (high && below >= confirmFrames)  { high = false; }
    }
    return count;
}

void PrintPressCal() {
    std::lock_guard<std::mutex> lk(g_calMutex);

    if (g_restAreas.size() < kRestSamples / 4) {
        printf("\nNot enough resting samples yet — keep a thumb on the pad,\n"
               "without pressing, until the second stage is announced.\n");
        return;
    }
    if (g_pressAreas.empty()) {
        puts("\nNo pressing stage recorded yet.");
        return;
    }

    std::vector<double> rest(g_restAreas.begin(), g_restAreas.end());
    const double restP50 = Percentile(rest, 0.50);
    const double restP95 = Percentile(rest, 0.95);
    const double restMax = Percentile(rest, 1.0);

    uint16_t peak = 0;
    for (uint16_t a : g_pressAreas) if (a > peak) peak = a;

    printf("\n===========================================================\n");
    printf("  Press detection calibration — %s pad\n",
           g_useLeftPad.load() ? "LEFT" : "RIGHT");
    printf("===========================================================\n\n");
    // Reported to say how much headroom the chosen threshold has, not to
    // derive a release level from — everything below the one threshold counts
    // as backed off, whatever a particular thumb rests at.
    printf("RESTING (n=%zu)   p50 %.0f   p95 %.0f   max %.0f\n",
           g_restAreas.size(), restP50, restP95, restMax);
    printf("PRESSING          peak %u   over %zu frames\n\n",
           peak, g_pressAreas.size());

    if (static_cast<double>(peak) <= restMax) {
        printf("A press never rose above the highest resting sample. Area cannot\n"
               "separate them at this grip, and detecting presses this way will\n"
               "not work.\n\n");
        return;
    }

    // One threshold, swept. The three columns are the same threshold with
    // different amounts of noise rejection, which is what says whether a bare
    // single threshold is enough or whether crossings need confirming.
    printf("You said %d presses. One threshold, no second release level.\n\n",
           g_calExpectedPresses);
    printf("  %-10s %-12s %-12s %s\n",
           "threshold", "no confirm", "2 frames", "3 frames");

    int bestT = 0;
    const int from = static_cast<int>(restP95);
    const int to   = static_cast<int>(peak);
    const int step = std::max(50, (to - from) / 24);
    for (int t = from; t <= to; t += step) {
        const int c1 = CountCrossings(g_pressAreas, t, 1);
        const int c2 = CountCrossings(g_pressAreas, t, 2);
        const int c3 = CountCrossings(g_pressAreas, t, 3);
        // The highest threshold that lands on the count with confirmation is
        // the pick: furthest from resting, so a heavy thumb has the most room
        // before it reads as a press.
        if (c3 == g_calExpectedPresses) bestT = t;
        printf("  %-10d %-12d %-12d %d%s\n", t, c1, c2, c3,
               c3 == g_calExpectedPresses ? "   <-- matches" : "");
    }

    printf("\n");
    if (bestT > 0) {
        printf("RECOMMENDED PRESS THRESHOLD: %d\n", bestT);
        printf("  %.0f above the highest resting sample, so a thumb lying on the\n"
               "  pad has that much headroom before it reads as a press.\n",
               bestT - restMax);
        const int bare = CountCrossings(g_pressAreas, bestT, 1);
        if (bare > g_calExpectedPresses) {
            printf("  At that threshold a bare crossing test reports %d rather than\n"
                   "  %d, so the area does chatter across the line and crossings\n"
                   "  need confirming. Three frames is 12ms.\n",
                   bare, g_calExpectedPresses);
        } else {
            printf("  A bare crossing test reports %d there too, so the signal is\n"
                   "  clean enough at this threshold that confirmation only costs\n"
                   "  latency. The simpler rule wins.\n", bare);
        }
    } else {
        printf("No threshold recovered exactly %d presses. The closest counts are\n"
               "in the table; if none is near, area and resting overlap at this\n"
               "grip and this approach will not work.\n", g_calExpectedPresses);
    }
    printf("\n");
}

void PrintTapSummary() {
    std::lock_guard<std::mutex> lk(g_tapMutex);
    const auto& taps = g_tap.taps;

    printf("\n===========================================================\n");
    printf("  Tap session — %s pad, %zu presses\n",
           g_useLeftPad.load() ? "LEFT" : "RIGHT", taps.size());
    printf("===========================================================\n\n");

    // First, and before any of the numbers below mean anything: was the stream
    // healthy? Printed even when nothing was captured, which is exactly the
    // case where the answer decides whether the session says anything at all.
    const double secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - g_tap.started).count();
    const double hz = secs > 0.0 ? static_cast<double>(g_tap.frames) / secs : 0.0;
    printf("STREAM: %lld frames over %.1fs = %.0f Hz\n", g_tap.frames, secs, hz);
    printf("  reports arrived %lld   read timeouts %lld\n",
           g_reportsSeen.load(), g_readTimeouts.load());
    printf("  discarded: wrong report id %lld%s, shorter than 30 bytes %lld\n",
           g_reportsWrongId.load(),
           g_lastWrongId.load() >= 0
               ? (" (last 0x" + std::to_string(g_lastWrongId.load()) + ")").c_str()
               : "",
           g_reportsShort.load());
    if (g_clicksDiscarded.load() > 0) {
        printf("  *** %lld discarded reports had a click bit set. Presses ARE\n"
               "      being thrown away by this tool, not missed by the pad. ***\n",
               g_clicksDiscarded.load());
    }
    if (g_tap.frames == 0) {
        printf("  Nothing arrived at all. The opened interface is not the one\n"
               "  carrying this controller's state.\n\n");
        return;
    }
    if (hz < 100.0) {
        printf("  Well under the controller's report rate — frames are being\n"
               "  dropped before this tool sees them, so presses can go missing\n"
               "  from this session that your thumb definitely made. Close the\n"
               "  tray app (two readers, two queues) and run the session again.\n");
    }
    printf("\n");

    if (taps.empty()) {
        printf("No presses seen. With the stream rate above in mind: at the full\n"
               "report rate the click bit genuinely never went high on this\n"
               "interface, which points at the wrong pad or the wrong slot.\n\n");
        return;
    }

    int fired = 0;
    for (const auto& t : taps) if (t.haptic) ++fired;
    printf("PRESS HAPTICS: %d of %zu presses would fire\n\n",
           fired, taps.size());

    // The question the click-bit count cannot answer on its own: were the
    // presses in the report at all?
    printf("PRESSES BY CONTACT AREA vs BY CLICK BIT\n");
    printf("  click bit rose        %zu times\n", taps.size());
    printf("  area crossed 3000     %d times\n", g_tap.spike3000.count);
    printf("  area crossed 3400     %d times\n", g_tap.spike3400.count);
    printf("  area crossed 3800     %d times\n", g_tap.spike3800.count);
    printf("  highest area seen     %u\n", g_tap.maxArea);
    printf("  If an area count matches the presses you made and the click count\n"
           "  does not, the presses are in the report and the firmware simply\n"
           "  is not calling them clicks — which is something we can act on.\n"
           "  If every count is low, the pad is not reporting the presses at\n"
           "  all and no amount of software will recover them.\n\n");

    printf("  %-4s %-7s %-7s %-11s %-5s %-9s %s\n",
           "#", "radius", "zone", "direction", "held", "lowFrames", "haptic");
    int i = 0;
    for (const auto& t : taps) {
        printf("  %-4d %-7.0f %-7s %-11s %-5d %-9d %s\n",
               ++i, t.radius, t.centre ? "CENTRE" : "ring",
               t.centre ? "(click)" : Sector8Name(Sector8(t.angle)),
               t.highFrames, t.lowFrames, t.haptic ? "yes" : "NO");
    }

    // Whether a press is held long enough for a game to notice it. Everything
    // this tool measures is on our side of the virtual pad; a game reading
    // XInput at 60Hz samples once every four frames, and a press shorter than
    // that can be delivered flawlessly and still never be seen.
    std::vector<double> held;
    for (const auto& t : taps) if (t.highFrames > 0) held.push_back(t.highFrames);
    if (!held.empty()) {
        printf("\nPRESS HELD, in frames (~4ms each)\n");
        printf("  min %.0f   p05 %.0f   p50 %.0f   max %.0f\n",
               Percentile(held, 0.0),  Percentile(held, 0.05),
               Percentile(held, 0.50), Percentile(held, 1.0));
        int risky = 0;
        for (double h : held) if (h < 5.0) ++risky;
        printf("  %d of %zu presses were under 5 frames (~20ms), which a game\n"
               "  polling at 60Hz can miss between two reads however cleanly\n"
               "  the press itself was dispatched.\n", risky, held.size());
    }

    // The question this answers: a press meant as a direction that landed
    // inside the ring produced the click binding instead, which feels exactly
    // like the press not registering.
    int centreCount = 0;
    std::vector<double> ringR, centreRadii;
    for (const auto& t : taps) {
        if (t.centre) { ++centreCount; centreRadii.push_back(t.radius); }
        else            ringR.push_back(t.radius);
    }
    printf("\nZONE (ring starts at %d)\n", kPadRingRadius);
    printf("  %d of %zu presses landed in the ring and resolved a direction\n",
           static_cast<int>(ringR.size()), taps.size());
    printf("  %d landed in the centre and were the click binding instead\n",
           centreCount);
    if (!ringR.empty()) {
        printf("  ring presses    r  min %.0f   p05 %.0f   p50 %.0f\n",
               Percentile(ringR, 0.0), Percentile(ringR, 0.05), Percentile(ringR, 0.50));
        printf("  the closest a direction press came to the boundary was %.0f above it\n",
               Percentile(ringR, 0.0) - kPadRingRadius);
    }
    if (!centreRadii.empty()) {
        printf("  centre presses  r  min %.0f   p50 %.0f   max %.0f\n",
               Percentile(centreRadii, 0.0), Percentile(centreRadii, 0.50),
               Percentile(centreRadii, 1.0));
        printf("  If those were meant as directions, the boundary is too far out.\n");
    }

    // The two questions the fix turns on: does the thumb ever get near the
    // idle area the latch waits for, and is a real release long enough to be
    // told apart from threshold chatter by duration alone?
    std::vector<double> gapAreas, lowRuns;
    for (const auto& t : taps) {
        if (t.minGapArea != 0xFFFF) gapAreas.push_back(t.minGapArea);
        if (t.gapClosed)            lowRuns.push_back(t.lowFrames);
    }

    if (!gapAreas.empty()) {
        // Kept because it is the evidence for why the latch stopped consulting
        // it: the release used to wait for this to fall to 1000, which a thumb
        // that stays on the pad never does.
        printf("\nCONTACT AREA BETWEEN PRESSES (what the latch used to wait on)\n");
        printf("  min %.0f   p05 %.0f   p50 %.0f   max %.0f\n",
               Percentile(gapAreas, 0.0),  Percentile(gapAreas, 0.05),
               Percentile(gapAreas, 0.50), Percentile(gapAreas, 1.0));
        int under = 0;
        for (double a : gapAreas) if (a <= 1000.0) ++under;
        printf("  %d of %zu gaps would have reached the old 1000 threshold\n",
               under, gapAreas.size());
    }

    // The other window that matters: how long a press spends touching before
    // it clicks. A tap has to outlast that to be told apart from one.
    std::vector<double> toClick;
    for (const auto& t : taps) if (t.touchToClick >= 0) toClick.push_back(t.touchToClick);
    if (!toClick.empty()) {
        printf("\nFINGER DOWN BEFORE THE CLICK, in frames (~4ms each)\n");
        printf("  min %.0f   p05 %.0f   p50 %.0f   max %.0f   (n=%zu)\n",
               Percentile(toClick, 0.0),  Percentile(toClick, 0.05),
               Percentile(toClick, 0.50), Percentile(toClick, 1.0), toClick.size());
        printf("  This is why a tap is decided on the lift rather than after a\n"
               "  fixed wait. Suppressing a tap on every press would need a wait\n"
               "  longer than the LONGEST of these (%.0f frames), which is no wait\n"
               "  at all — a thumb gets planted and then pressed.\n",
               Percentile(toClick, 1.0));
    }

    if (!lowRuns.empty()) {
        printf("\nCLICK BIT LOW between presses, in frames (~4ms each)\n");
        printf("  min %.0f   p05 %.0f   p50 %.0f   max %.0f\n",
               Percentile(lowRuns, 0.0),  Percentile(lowRuns, 0.05),
               Percentile(lowRuns, 0.50), Percentile(lowRuns, 1.0));
        printf("  A release confirmed on the click bit alone would need a\n"
               "  window under %.0f frames to catch every one of these.\n",
               Percentile(lowRuns, 0.0));
    }
    printf("\n");
}

// Reader thread
// ---------------------------------------------------------------------------

void RecordPress(const PadHistory& hist, uint16_t area) {
    const PadHistory::Frame& at = hist.Back(0);

    Press p;
    p.stage       = g_stage.load();
    p.x           = at.x;
    p.y           = at.y;
    p.r           = Radius(at.x, at.y);
    p.angle       = AngleDeg(at.x, at.y);
    p.areaAtClick = area;

    // Walk back through the history. Only frames where the pad was actually
    // being touched carry a meaningful position — a stab that lands and clicks
    // in the same frame has nothing behind it, and reporting the stale
    // coordinates from the previous touch as drift would be a fabrication.
    for (size_t k = 1; k <= kMaxLookback; ++k) {
        const PadHistory::Frame& prev = hist.Back(k);
        if (!prev.touching) { p.drift[k] = -1.0; continue; }
        p.drift[k] = std::hypot(static_cast<double>(at.x - prev.x),
                                static_cast<double>(at.y - prev.y));
        if (k == 3) p.areaPre = prev.area;  // the lookback the plan starts from
    }

    const int sector8 = Sector8(p.angle);
    const int sector4 = Sector4(p.angle);

    int    stageNow = -1;
    double maxR     = 0.0;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_presses.push_back(p);
        maxR = g_maxTouchRadius;
        stageNow = p.stage;
    }

    const double frac = maxR > 0.0 ? p.r / maxR : 0.0;
    printf("\n  click  x=%6d y=%6d   r=%7.0f (%.2f of max)   %6.1f deg"
           "   8-way %-10s 4-way %-6s  area %u",
           p.x, p.y, p.r, frac, p.angle,
           Sector8Name(sector8), Sector4Name(sector4), p.areaAtClick);
    if (p.drift[3] >= 0.0) printf("   drift@3 %.0f", p.drift[3]);
    printf("\n");

    // Advance the guided sequence once this stage has its samples.
    if (stageNow >= 0) {
        const int done = g_stageDone.fetch_add(1) + 1;
        if (done >= kStages[stageNow].reps) {
            const int next = stageNow + 1;
            g_stageDone.store(0);
            if (next >= kStageCount) {
                g_stage.store(-1);
                puts("\nCalibration complete.");
                PrintSummary();
                printf("> ");
                fflush(stdout);
            } else {
                g_stage.store(next);
                PrintStagePrompt(next);
            }
            return;
        }
        printf("    %d of %d\n> ", done, kStages[stageNow].reps);
    } else {
        printf("> ");
    }
    fflush(stdout);
}

void ReaderThread(HidDevice* dev) {
    PadHistory left, right;
    bool prevLeftClick = false, prevRightClick = false;

    while (!g_quit.load()) {
        uint8_t buf[64] = {};
        const size_t n = dev->ReadInputReport(buf, sizeof(buf), 16);
        if (n == 0) { ++g_readTimeouts; continue; }
        ++g_reportsSeen;
        // Everything discarded here is discarded before the tap tracker sees
        // it, and a press hidden in a discarded report is indistinguishable
        // from a press that never happened. Counted rather than assumed: a
        // session that reports fewer presses than the thumb made has to be
        // able to say whether this is where they went.
        if (!SteamController::IsStateReportId(buf[0])) {
            ++g_reportsWrongId;
            g_lastWrongId = buf[0];
            continue;
        }
        // Coordinates start at 18 and the right pad's contact area ends at 30 —
        // the same threshold TrackpadInput requires before it reads anything.
        if (n < 30) {
            ++g_reportsShort;
            // A short report still carries the button bytes, so it can still
            // say whether a click was in it that this tool then threw away.
            if (n > 4 && (buf[4] & SteamController::BTN_TP_RT_CLICK)) ++g_clicksDiscarded;
            if (n > 5 && (buf[5] & SteamController::BTN_TP_LT_CLICK)) ++g_clicksDiscarded;
            continue;
        }

        const uint8_t b2 = buf[4];
        const uint8_t b3 = buf[5];

        PadHistory::Frame lf, rf;
        lf.touching = (b3 & SteamController::BTN_TP_LT) != 0;
        rf.touching = (b2 & SteamController::BTN_TP_RT) != 0;
        std::memcpy(&lf.x,    buf + 18, 2);
        std::memcpy(&lf.y,    buf + 20, 2);
        std::memcpy(&lf.area, buf + 22, 2);
        std::memcpy(&rf.x,    buf + 24, 2);
        std::memcpy(&rf.y,    buf + 26, 2);
        std::memcpy(&rf.area, buf + 28, 2);

        left.Push(lf);
        right.Push(rf);

        // The usable radius is whatever a thumb reached while merely touching —
        // a click cannot be aimed anywhere a touch could not go first.
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (lf.touching) g_maxTouchRadius = std::max(g_maxTouchRadius, Radius(lf.x, lf.y));
            if (rf.touching) g_maxTouchRadius = std::max(g_maxTouchRadius, Radius(rf.x, rf.y));
        }

        const bool leftClick  = (b3 & SteamController::BTN_TP_LT_CLICK) != 0;
        const bool rightClick = (b2 & SteamController::BTN_TP_RT_CLICK) != 0;
        const bool wantLeft   = g_useLeftPad.load();

        const int cal = g_pressCal.load();
        if (cal != static_cast<int>(PressCal::Off)) {
            const bool touching = wantLeft ? lf.touching : rf.touching;
            const uint16_t area = wantLeft ? lf.area : rf.area;
            std::lock_guard<std::mutex> lk(g_calMutex);
            if (cal == static_cast<int>(PressCal::Resting)) {
                if (touching) g_restAreas.push_back(area);
                if (g_restAreas.size() >= kRestSamples) {
                    g_pressCal.store(static_cast<int>(PressCal::Pressing));
                    printf("\n  Resting captured. Now press %d times, the way you\n"
                           "  actually play. Then 'sum'.\n> ", g_calExpectedPresses);
                    fflush(stdout);
                }
            } else {
                // Untouched frames go in as zero so the detector below sees a
                // thumb leaving the pad as the release it is.
                g_pressAreas.push_back(touching ? area : 0);
            }
        }

        // Tap mode replays the latch frame by frame, so it needs every frame,
        // not just the click edges the zone recorder cares about.
        if (g_tapMode.load()) {
            std::lock_guard<std::mutex> lk(g_tapMutex);
            g_tap.Frame(wantLeft ? lf.touching : rf.touching,
                        wantLeft ? leftClick   : rightClick,
                        wantLeft ? lf.area     : rf.area,
                        wantLeft ? lf.x        : rf.x,
                        wantLeft ? lf.y        : rf.y);
        } else {
            if (wantLeft && leftClick && !prevLeftClick)   RecordPress(left,  lf.area);
            if (!wantLeft && rightClick && !prevRightClick) RecordPress(right, rf.area);
        }

        prevLeftClick  = leftClick;
        prevRightClick = rightClick;
    }
}

// ---------------------------------------------------------------------------

void PrintHelp() {
    printf(
        "\nTrackpad Zone Probe\n"
        "Reads only — never writes to the controller, never touches lizard mode,\n"
        "so Steam can stay running.\n"
        "\n"
        "Every click on the selected pad prints where it landed. Run 'cal' for a\n"
        "guided sequence that ends with a recommended ring boundary.\n"
        "\n"
        "Commands:\n"
        "  cal          start the guided calibration (%d stages)\n"
        "  tap          record rapid presses and replay the haptic latch over\n"
        "               them, to show which presses would fire a haptic\n"
        "  presscal [n] find the contact area that separates a resting thumb\n"
        "               from a pressing one, since the firmware's click bit\n"
        "               misses about half of them (default 15 presses)\n"
        "  stop         abandon a calibration in progress\n"
        "  pad l|r      choose which pad to watch (currently %s)\n"
        "  sum          print the summary for what has been collected so far\n"
        "  reset        discard all samples and start over\n"
        "  ?            this help\n"
        "  q            quit\n\n",
        kStageCount, g_useLeftPad.load() ? "LEFT" : "RIGHT");
}

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream ss(line);
    std::string tok;
    while (ss >> tok) tokens.push_back(tok);
    return tokens;
}

}  // namespace

int main() {
    auto paths = SteamController::EnumerateAll();
    if (paths.empty()) {
        puts("No Steam Controller found (wired PID=1302 or dongle PID=1304).");
        return 1;
    }

    HidDevice dev;
    if (!dev.Open(paths[0])) {
        puts("Failed to open HID device.");
        return 1;
    }
    printf("Opened %ls\n", paths[0].c_str());
    // A puck publishes an interface per slot and only one has a controller in
    // it, so the first path is not always the live one. Say when there was a
    // choice, since "no presses recorded" and "watching an empty slot" look
    // identical from the outside.
    if (paths.size() > 1) {
        printf("NOTE: %zu interfaces found; this is the first. If presses do not\n"
               "      register, the controller is on one of the others.\n",
               paths.size());
    }

    PrintHelp();

    std::thread reader(ReaderThread, &dev);

    char lineBuf[256];
    while (true) {
        printf("> ");
        fflush(stdout);
        if (!fgets(lineBuf, sizeof(lineBuf), stdin)) break;

        const auto toks = Split(lineBuf);
        if (toks.empty()) continue;
        const std::string& cmd = toks[0];

        if (cmd == "q" || cmd == "quit") break;

        if (cmd == "?" || cmd == "help") {
            PrintHelp();
            continue;
        }

        if (cmd == "cal") {
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                g_presses.clear();
            }
            g_stageDone.store(0);
            g_stage.store(0);
            printf("\nPress the pad the way you would while playing — same grip,\n"
                   "same thumb, no aiming for the sensor. Nine stages.\n");
            PrintStagePrompt(0);
            continue;
        }

        if (cmd == "stop") {
            g_stage.store(-1);
            g_stageDone.store(0);
            g_tapMode.store(false);
            g_pressCal.store(static_cast<int>(PressCal::Off));
            puts("Stopped. Samples so far are kept — 'sum' to see them.");
            continue;
        }

        if (cmd == "pad") {
            if (toks.size() < 2 || (toks[1] != "l" && toks[1] != "r")) {
                puts("usage: pad l | pad r");
                continue;
            }
            g_useLeftPad.store(toks[1] == "l");
            printf("Watching the %s pad.\n", g_useLeftPad.load() ? "LEFT" : "RIGHT");
            continue;
        }

        if (cmd == "tap") {
            {
                std::lock_guard<std::mutex> lk(g_tapMutex);
                g_tap = TapTracker{};
            }
            g_reportsSeen.store(0);
            g_reportsWrongId.store(0);
            g_reportsShort.store(0);
            g_clicksDiscarded.store(0);
            g_readTimeouts.store(0);
            g_lastWrongId.store(-1);
            g_tapMode.store(true);
            printf("\nPress the pad the way you actually play. Each press prints\n"
                   "where it landed and what a directional pad made of it, so a\n"
                   "press that did not do what you meant is visible as it happens.\n"
                   "Then 'sum'.\n");
            continue;
        }

        if (cmd == "presscal") {
            if (toks.size() >= 2) {
                const int want = std::atoi(toks[1].c_str());
                if (want > 0) g_calExpectedPresses = want;
            }
            {
                std::lock_guard<std::mutex> lk(g_calMutex);
                g_restAreas.clear();
                g_pressAreas.clear();
            }
            g_tapMode.store(false);
            g_pressCal.store(static_cast<int>(PressCal::Resting));
            printf("\nStage 1 of 2. Rest your thumb on the pad WITHOUT pressing,\n"
                   "the way it sits between presses. Hold for about five seconds;\n"
                   "the next stage announces itself.\n");
            continue;
        }

        if (cmd == "sum" || cmd == "summary") {
            if (g_pressCal.load() != static_cast<int>(PressCal::Off)) PrintPressCal();
            else if (g_tapMode.load())                                PrintTapSummary();
            else                                                      PrintSummary();
            continue;
        }

        if (cmd == "reset") {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_presses.clear();
            g_maxTouchRadius = 0.0;
            puts("Samples discarded.");
            continue;
        }

        printf("Unknown command '%s'. '?' for help.\n", cmd.c_str());
    }

    g_quit.store(true);
    reader.join();
    return 0;
}
