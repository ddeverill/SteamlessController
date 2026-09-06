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
        if (n == 0) continue;
        if (!SteamController::IsStateReportId(buf[0])) continue;
        // Coordinates start at 18 and the right pad's contact area ends at 30 —
        // the same threshold TrackpadInput requires before it reads anything.
        if (n < 30) continue;

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

        if (wantLeft && leftClick && !prevLeftClick)   RecordPress(left,  lf.area);
        if (!wantLeft && rightClick && !prevRightClick) RecordPress(right, rf.area);

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
            puts("Calibration abandoned. Samples so far are kept — 'sum' to see them.");
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

        if (cmd == "sum" || cmd == "summary") {
            PrintSummary();
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
