// TriggerProbe — measures what the Steam Controller 2026's triggers actually
// report, and which haptic is the best stand-in for the click they do not have,
// so a simulated dual-stage trigger (issue #75) starts from numbers rather than
// guesses.
//
// The hardware has smooth Hall-effect triggers with no physical second stage, so
// "full press" would be a software threshold on the analog value. That needs
// answers this tool produces and nothing else in the tree does:
//
//   1. Where the trigger rests and how noisy it is. The full-press point and its
//      release hysteresis have to clear this by a wide margin or the binding
//      chatters.
//   2. How far a deliberate full pull actually travels, and whether that is the
//      same every time. A hard stop gives a flat, repeatable top end that a high
//      threshold can sit under safely. A soft bottom that varies pull to pull
//      forces the threshold lower, or the lightest full pull never triggers.
//   3. How fast a fast pull moves, which is the headroom the detector has and
//      what any "quick pull skips the soft stage" timing would have to live in.
//   4. How steady a held half-pull is — the tremor a soft-stage threshold's
//      hysteresis would have to exceed. The full press is pinned against the end
//      of travel and does not wobble like this.
//   5. Whether the firmware's full-pull bits (BTN_RT_FULL, BTN_LT_FULL) mean
//      anything, and where each fires and drops. The left one was found with
//      this tool's bit scan, which correlates every button bit against the
//      trigger being fully pulled and lists the ones that track it.
//   6. Which haptic reads as a click under the trigger finger. The only haptics
//      the app fires today are the trackpad actuators, which sit under the
//      thumbs; the grip motors are nearer the trigger finger. This tool fires
//      each candidate on a threshold crossing and times how long the host takes
//      to get it out, because a BLE write can block for far longer than a USB one
//      and a click that arrives late is worse than none.
//
// Non-invasive by construction, like TrackpadZoneProbe: opens the device shared,
// never touches lizard mode, and the only reports it sends are the haptic ones
// the user asks for. Steam can keep running.
//
// Timings are host arrival times, not firmware timestamps, so over Bluetooth
// (~157 Hz, batched) they are coarser than over USB or the puck (~250 Hz). Run it
// on the transport you care about.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <conio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "hid/HidDevice.h"
#include "steam/SteamController.h"

namespace {

// Raw trigger range. The report documents 0x0000..0x7FFF; whether a given pull
// actually reaches 0x7FFF is one of the things this tool measures.
constexpr double kFull = 32767.0;

double Pct(double raw) { return raw / kFull * 100.0; }

double NowMs() {
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = p / 100.0 * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(idx));
    const size_t hi = std::min(lo + 1, v.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

struct Stats {
    size_t n = 0;
    double min = 0, med = 0, p99 = 0, max = 0, mean = 0, sd = 0;
};

Stats Compute(const std::vector<double>& v) {
    Stats s;
    s.n = v.size();
    if (v.empty()) return s;
    s.min  = *std::min_element(v.begin(), v.end());
    s.max  = *std::max_element(v.begin(), v.end());
    s.med  = Percentile(v, 50.0);
    s.p99  = Percentile(v, 99.0);
    double sum = 0.0;
    for (double x : v) sum += x;
    s.mean = sum / static_cast<double>(v.size());
    double sq = 0.0;
    for (double x : v) sq += (x - s.mean) * (x - s.mean);
    s.sd = std::sqrt(sq / static_cast<double>(v.size()));
    return s;
}

// ---------------------------------------------------------------------------
// Recorded data
// ---------------------------------------------------------------------------

struct Sample {
    double   tMs = 0.0;
    uint16_t val = 0;
    // buf[2..5], the four button bytes, kept whole so the bit scan can look at
    // every one of them rather than only the bits the header names.
    uint8_t  b[4] = {};
};

// One recorded gesture: a pull, a hold, or a stretch of rest.
struct Rep {
    int                 stage = 0;
    std::vector<Sample> s;
};

enum class Kind { Rest, Slow, Fast, Half, Squeeze };

struct StageDef {
    const char* name;
    const char* prompt;
    Kind        kind;
    int         reps;
};

const StageDef kStages[] = {
    { "REST",    "Take your finger OFF the trigger and keep it off (3 seconds)", Kind::Rest,    1 },
    { "SLOW",    "Pull SLOWLY all the way down, then let it back up slowly",      Kind::Slow,    5 },
    { "FAST",    "Stab it all the way down FAST, then release it fast",           Kind::Fast,    5 },
    { "HALF",    "Pull to about HALFWAY and hold it as steady as you can (4 s)",  Kind::Half,    1 },
    { "SQUEEZE", "Pull all the way down and squeeze HARD for 3 seconds",          Kind::Squeeze, 2 },
};
constexpr int kStageCount = static_cast<int>(sizeof(kStages) / sizeof(kStages[0]));

const char* SideName(int side) { return side == 0 ? "LEFT" : "RIGHT"; }

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

std::mutex        g_mutex;
std::vector<Rep>  g_reps[2];  // 0 = left trigger, 1 = right
std::atomic<bool> g_quit{false};

// Latest values, for the live view and the haptic detector.
std::atomic<int>  g_val[2]{ 0, 0 };
std::atomic<int>  g_peak[2]{ 0, 0 };
std::atomic<bool> g_fullBit[2]{ false, false };  // firmware full-pull bits, left then right

std::atomic<uint64_t> g_reportsSeen{0};
std::atomic<uint64_t> g_reportsWrongId{0};
std::atomic<uint64_t> g_reportsShort{0};

// Calibration state machine. Driven frame by frame from the reader thread so no
// frame can fall between stages; guarded by g_mutex.
enum class Phase { WaitRelease, Armed, Recording };

struct Cal {
    bool   active = false;
    int    side = 0;
    int    stage = 0;
    int    repsDone = 0;
    Phase  phase = Phase::WaitRelease;
    double armedAtMs = 0.0;
    double recStartMs = 0.0;
    int    peak = 0;
    Rep    cur;
} g_cal;

void PrintStagePrompt(int stage) {
    if (stage < 0 || stage >= kStageCount) return;
    printf("\n[%d/%d] %s  (%d time%s)\n> ",
           stage + 1, kStageCount, kStages[stage].prompt,
           kStages[stage].reps, kStages[stage].reps == 1 ? "" : "s");
    fflush(stdout);
}

// Called with g_mutex held, once per frame, with the frame for the trigger under
// calibration.
void FinishRep(bool keep) {
    Cal& c = g_cal;
    if (keep) {
        c.cur.stage = c.stage;
        g_reps[c.side].push_back(std::move(c.cur));
        ++c.repsDone;
        if (kStages[c.stage].reps > 1)
            printf("\n  ok (%d/%d)\n", c.repsDone, kStages[c.stage].reps);
    }
    c.cur = Rep{};
    c.peak = 0;
    c.phase = Phase::WaitRelease;
    if (keep && c.repsDone >= kStages[c.stage].reps) {
        ++c.stage;
        c.repsDone = 0;
        if (c.stage >= kStageCount) {
            c.active = false;
            printf("\nDone. 'sum' shows the results.\n> ");
            fflush(stdout);
            return;
        }
        PrintStagePrompt(c.stage);
        return;
    }
    printf("> ");
    fflush(stdout);
}

void CalStep(const Sample& s) {
    Cal& c = g_cal;
    const Kind kind = kStages[c.stage].kind;
    const double v = s.val;
    const double lo = 0.03 * kFull;

    switch (c.phase) {
    case Phase::WaitRelease:
        // Every stage starts from a released trigger, so the previous gesture's
        // tail cannot be mistaken for the next one's start.
        if (v < lo) { c.phase = Phase::Armed; c.armedAtMs = s.tMs; }
        return;

    case Phase::Armed: {
        bool start = false;
        switch (kind) {
        // Rest waits a beat so the finger has actually left before recording.
        case Kind::Rest:    start = (s.tMs - c.armedAtMs) >= 1500.0; break;
        case Kind::Slow:
        case Kind::Fast:    start = v >= lo; break;
        case Kind::Half:    start = v >= 0.15 * kFull; break;
        case Kind::Squeeze: start = v >= 0.80 * kFull; break;
        }
        if (!start) return;
        c.phase = Phase::Recording;
        c.recStartMs = s.tMs;
        c.peak = 0;
        c.cur = Rep{};
        [[fallthrough]];
    }

    case Phase::Recording: {
        c.cur.s.push_back(s);
        c.peak = std::max(c.peak, static_cast<int>(s.val));
        const double elapsed = s.tMs - c.recStartMs;
        switch (kind) {
        case Kind::Rest:    if (elapsed >= 3000.0) FinishRep(true); break;
        case Kind::Half:    if (elapsed >= 4000.0) FinishRep(true); break;
        case Kind::Squeeze: if (elapsed >= 3000.0) FinishRep(true); break;
        case Kind::Slow:
        case Kind::Fast:
            // A pull ends when the trigger comes back up. One that never got
            // past halfway is a hesitation, not a pull — thrown away so it does
            // not drag the peaks down.
            if (v < lo && c.cur.s.size() > 1) {
                if (c.peak >= static_cast<int>(0.5 * kFull)) {
                    FinishRep(true);
                } else {
                    printf("\n  that one did not get past halfway — again\n");
                    FinishRep(false);
                }
            }
            break;
        }
        return;
    }
    }
}

// ---------------------------------------------------------------------------
// Haptic candidates
// ---------------------------------------------------------------------------

// Side byte on the haptic output reports, as measured on hardware and recorded
// in SteamController.cpp: 0 left pad, 1 right pad, 2 both.
constexpr uint8_t kPadLeft = 0, kPadRight = 1, kPadBoth = 2;
constexpr uint8_t kCmdTick = 1, kCmdClick = 2;

void WriteU16LE(uint8_t* p, uint16_t v) { std::memcpy(p, &v, sizeof(v)); }

bool SendCommand(HidDevice& dev, uint8_t side, uint8_t cmd, int8_t gainDb) {
    uint8_t buf[4] = {};
    buf[0] = SteamController::OUT_HAPTIC_COMMAND;
    buf[1] = side;
    buf[2] = cmd;
    buf[3] = static_cast<uint8_t>(gainDb);
    return dev.WriteOutputReport(buf, sizeof(buf));
}

bool SendRumble(HidDevice& dev, uint16_t leftSpeed, uint16_t rightSpeed) {
    uint8_t buf[10] = {};
    buf[0] = SteamController::OUT_HAPTIC_RUMBLE;
    buf[1] = 0x00;
    WriteU16LE(buf + 2, 0x0000);
    WriteU16LE(buf + 4, leftSpeed);
    buf[6] = 0x00;
    WriteU16LE(buf + 7, rightSpeed);
    buf[9] = 0x00;
    return dev.WriteOutputReport(buf, sizeof(buf));
}

struct Candidate {
    const char* name;
    const char* what;
};

// Index 0 is unused so the numbers the user types are the numbers in the table.
const Candidate kCandidates[] = {
    { "", "" },
    { "pad-click",       "firmware CLICK on the pad under the same thumb" },
    { "pad-tick",        "firmware TICK (lighter) on the pad under the same thumb" },
    { "pad-click-both",  "firmware CLICK on both pads" },
    { "grip-buzz",       "60 ms buzz in the grip on the same side as the trigger" },
    { "grip-buzz-hard",  "90 ms strong buzz in the grip on the same side" },
    { "pad+grip",        "pad CLICK and a grip buzz together, same side" },
    { "grip-both",       "60 ms buzz in both grips" },
};
constexpr int kCandidateCount = static_cast<int>(sizeof(kCandidates) / sizeof(kCandidates[0])) - 1;

// The rumble channel is a level, not an event: the app's own RumbleLoop re-sends
// it every 40 ms because the actuators need continuous drive. A burst here does
// the same for its duration, then sends zero twice in case one is dropped.
void Burst(HidDevice& dev, bool left, bool right, uint16_t speed, int durMs, double& tFirst) {
    const double start = NowMs();
    bool first = true;
    while (NowMs() - start < durMs) {
        SendRumble(dev, left ? speed : uint16_t{0}, right ? speed : uint16_t{0});
        if (first) { tFirst = NowMs(); first = false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    SendRumble(dev, 0, 0);
    SendRumble(dev, 0, 0);
}

// side: 0 = the left trigger's, 1 = the right's, 2 = both (manual 'fire ... b').
// tFirst is stamped when the first report has been handed to the driver, which
// is the moment that decides how late the click arrives.
void Fire(HidDevice& dev, int cand, int side, double& tFirst) {
    const bool l = (side == 0 || side == 2);
    const bool r = (side == 1 || side == 2);
    const uint8_t pad = side == 2 ? kPadBoth : (side == 0 ? kPadLeft : kPadRight);
    switch (cand) {
    case 1: SendCommand(dev, pad, kCmdClick, 0); tFirst = NowMs(); break;
    case 2: SendCommand(dev, pad, kCmdTick, 0);  tFirst = NowMs(); break;
    case 3: SendCommand(dev, kPadBoth, kCmdClick, 0); tFirst = NowMs(); break;
    case 4: Burst(dev, l, r, 0x5000, 60, tFirst); break;
    case 5: Burst(dev, l, r, 0xB000, 90, tFirst); break;
    case 6:
        SendCommand(dev, pad, kCmdClick, 0);
        tFirst = NowMs();
        { double ignored = 0.0; Burst(dev, l, r, 0x5000, 60, ignored); }
        break;
    case 7: Burst(dev, true, true, 0x5000, 60, tFirst); break;
    default: break;
    }
}

// Haptic worker. The reader thread only queues an event and goes on reading: a
// write that blocks (BLE can, for seconds, on a bad link) must not stall input,
// and the delay it adds is exactly what this measures — the stamp the reader
// puts on the event is the crossing, the stamp the worker takes is the write
// returning.
struct HapticEvent {
    int    cand = 0;
    int    side = 0;
    double tCrossMs = 0.0;
};

std::mutex                g_hMutex;
std::condition_variable   g_hCv;
std::deque<HapticEvent>   g_hQueue;
std::vector<double>       g_hLatencyMs;  // crossing -> first report written
std::map<int, int>        g_ratings;

std::atomic<bool> g_armOn{false};
std::atomic<int>  g_armCand{1};
std::atomic<int>  g_armPress{0};    // raw
std::atomic<int>  g_armRelease{0};  // raw
std::atomic<bool> g_armOnRelease{true};
std::atomic<bool> g_down[2]{ false, false };

void HapticWorker(HidDevice* dev) {
    while (true) {
        HapticEvent ev;
        {
            std::unique_lock<std::mutex> lk(g_hMutex);
            g_hCv.wait(lk, [] { return g_quit.load() || !g_hQueue.empty(); });
            if (g_quit.load()) return;
            ev = g_hQueue.front();
            g_hQueue.pop_front();
        }
        double tFirst = NowMs();
        Fire(*dev, ev.cand, ev.side, tFirst);
        std::lock_guard<std::mutex> lk(g_hMutex);
        g_hLatencyMs.push_back(tFirst - ev.tCrossMs);
    }
}

void QueueHaptic(int cand, int side, double tCrossMs) {
    {
        std::lock_guard<std::mutex> lk(g_hMutex);
        g_hQueue.push_back({ cand, side, tCrossMs });
    }
    g_hCv.notify_one();
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

uint16_t ClampRaw(int16_t v) { return v < 0 ? uint16_t{0} : static_cast<uint16_t>(v); }

void ReaderThread(HidDevice* dev) {
    while (!g_quit.load()) {
        uint8_t buf[64] = {};
        const size_t n = dev->ReadInputReport(buf, sizeof(buf), 16);
        if (n == 0) continue;
        ++g_reportsSeen;
        if (!SteamController::IsStateReportId(buf[0])) { ++g_reportsWrongId; continue; }
        // Triggers end at buf[9]; a report shorter than that has none to read.
        if (n < 10) { ++g_reportsShort; continue; }

        const double t = NowMs();
        int16_t ltRaw = 0, rtRaw = 0;
        std::memcpy(&ltRaw, buf + 6, 2);
        std::memcpy(&rtRaw, buf + 8, 2);
        const uint16_t vals[2] = { ClampRaw(ltRaw), ClampRaw(rtRaw) };

        for (int i = 0; i < 2; ++i) {
            g_val[i].store(vals[i]);
            if (vals[i] > g_peak[i].load()) g_peak[i].store(vals[i]);
        }
        g_fullBit[0].store((buf[5] & SteamController::BTN_LT_FULL) != 0);
        g_fullBit[1].store((buf[4] & SteamController::BTN_RT_FULL) != 0);

        if (g_cal.active) {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_cal.active) {
                Sample s;
                s.tMs = t;
                s.val = vals[g_cal.side];
                for (int k = 0; k < 4; ++k) s.b[k] = buf[2 + k];
                CalStep(s);
            }
        }

        if (g_armOn.load()) {
            const int press = g_armPress.load();
            const int release = g_armRelease.load();
            for (int i = 0; i < 2; ++i) {
                const bool down = g_down[i].load();
                if (!down && vals[i] >= press) {
                    g_down[i].store(true);
                    QueueHaptic(g_armCand.load(), i, t);
                } else if (down && vals[i] <= release) {
                    g_down[i].store(false);
                    if (g_armOnRelease.load()) QueueHaptic(g_armCand.load(), i, t);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------

std::vector<double> Vals(const Rep& r) {
    std::vector<double> v;
    v.reserve(r.s.size());
    for (const auto& s : r.s) v.push_back(s.val);
    return v;
}

int PeakOf(const Rep& r) {
    int p = 0;
    for (const auto& s : r.s) p = std::max(p, static_cast<int>(s.val));
    return p;
}

// Index of the first sample at or after `from` satisfying val >= level, or -1.
int FirstAtOrAbove(const Rep& r, size_t from, double level) {
    for (size_t i = from; i < r.s.size(); ++i)
        if (r.s[i].val >= level) return static_cast<int>(i);
    return -1;
}

int FirstAtOrBelow(const Rep& r, size_t from, double level) {
    for (size_t i = from; i < r.s.size(); ++i)
        if (r.s[i].val <= level) return static_cast<int>(i);
    return -1;
}

void PrintSpan(const char* label, const std::vector<double>& v, const char* unit) {
    if (v.empty()) { printf("  %-28s (no data)\n", label); return; }
    const Stats s = Compute(v);
    printf("  %-28s min %6.1f  median %6.1f  max %6.1f %s   (n=%zu)\n",
           label, s.min, s.med, s.max, unit, s.n);
}

void Summarize(int side) {
    std::vector<Rep> reps;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        reps = g_reps[side];
    }
    if (reps.empty()) {
        printf("No %s trigger data. Run 'cal %c' first.\n", SideName(side), side == 0 ? 'l' : 'r');
        return;
    }

    printf("\n==== %s TRIGGER ====\n", SideName(side));

    // Report rate, from the gaps inside recordings only so a pause between
    // stages does not count as a slow report.
    std::vector<double> gaps;
    for (const auto& r : reps)
        for (size_t i = 1; i < r.s.size(); ++i) gaps.push_back(r.s[i].tMs - r.s[i - 1].tMs);
    if (!gaps.empty()) {
        const double med = Percentile(gaps, 50.0);
        printf("Report cadence: median gap %.1f ms (~%.0f Hz), p99 gap %.1f ms\n",
               med, med > 0.0 ? 1000.0 / med : 0.0, Percentile(gaps, 99.0));
    }

    // 1. Rest ------------------------------------------------------------------
    std::vector<double> rest;
    for (const auto& r : reps)
        if (kStages[r.stage].kind == Kind::Rest)
            for (const auto& s : r.s) rest.push_back(s.val);
    double restCeil = 0.0;
    printf("\n1. REST (finger off)\n");
    if (rest.empty()) {
        puts("   (no rest data)");
    } else {
        const Stats s = Compute(rest);
        restCeil = Pct(s.max);
        printf("   raw min %.0f  median %.0f  p99 %.0f  max %.0f   =>  noise ceiling %.2f%%\n",
               s.min, s.med, s.p99, s.max, restCeil);
    }

    // 2. Travel ----------------------------------------------------------------
    std::vector<double> slowPeaks, fastPeaks, squeezePeaks, allPeaks;
    for (const auto& r : reps) {
        const double p = PeakOf(r);
        switch (kStages[r.stage].kind) {
        case Kind::Slow:    slowPeaks.push_back(p);    allPeaks.push_back(p); break;
        case Kind::Fast:    fastPeaks.push_back(p);    allPeaks.push_back(p); break;
        case Kind::Squeeze: squeezePeaks.push_back(p); allPeaks.push_back(p); break;
        default: break;
        }
    }
    printf("\n2. FULL TRAVEL (peak of each deliberate pull, %% of 0x7FFF)\n");
    auto pctVec = [](const std::vector<double>& v) {
        std::vector<double> o;
        for (double x : v) o.push_back(Pct(x));
        return o;
    };
    PrintSpan("slow pulls",    pctVec(slowPeaks),    "%");
    PrintSpan("fast stabs",    pctVec(fastPeaks),    "%");
    PrintSpan("hard squeezes", pctVec(squeezePeaks), "%");
    double minPeak = 0.0;
    if (!allPeaks.empty()) {
        const Stats s = Compute(allPeaks);
        minPeak = s.min;
        const double spread = Pct(s.max - s.min);
        printf("   all pulls: weakest %.1f%%, strongest %.1f%%, spread %.1f points\n",
               Pct(s.min), Pct(s.max), spread);
        if (spread < 2.0)
            puts("   -> peaks agree to ~2 points: the trigger has a firm bottom. A high threshold is safe.");
        else if (spread < 8.0)
            puts("   -> modest spread: the bottom is a little soft. Keep the threshold under the weakest pull.");
        else
            puts("   -> wide spread: no firm bottom. The threshold has to sit well under the weakest pull\n"
                 "      or some deliberate full pulls will never register.");
    }

    // Plateau: how flat the top of a squeeze is. This, not the half-pull wobble,
    // is what a full-press release has to be wider than — the finger is pinned
    // against the end of travel, not hovering mid-range.
    double plateauRange = 0.0;
    for (const auto& r : reps) {
        if (kStages[r.stage].kind != Kind::Squeeze) continue;
        const int pk = PeakOf(r);
        std::vector<double> top;
        for (const auto& s : r.s)
            if (s.val >= 0.9 * pk) top.push_back(s.val);
        if (top.size() < 4) continue;
        const Stats ts = Compute(top);
        plateauRange = std::max(plateauRange, Pct(ts.max - ts.min));
        printf("   squeeze plateau: %zu frames within 10%% of its peak, range %.2f points, sd %.2f\n",
               ts.n, Pct(ts.max - ts.min), Pct(ts.sd));
    }

    // 3. Speed -----------------------------------------------------------------
    std::vector<double> rise, fall;
    for (const auto& r : reps) {
        if (kStages[r.stage].kind != Kind::Fast) continue;
        const double pk = PeakOf(r);
        const int a = FirstAtOrAbove(r, 0, 0.1 * pk);
        if (a < 0) continue;
        const int b = FirstAtOrAbove(r, static_cast<size_t>(a), 0.9 * pk);
        if (b < 0) continue;
        rise.push_back(r.s[static_cast<size_t>(b)].tMs - r.s[static_cast<size_t>(a)].tMs);
        // Peak index, then the way back down.
        size_t pi = 0;
        for (size_t i = 0; i < r.s.size(); ++i) if (r.s[i].val == static_cast<uint16_t>(pk)) { pi = i; break; }
        const int c = FirstAtOrBelow(r, pi, 0.9 * pk);
        if (c < 0) continue;
        const int d = FirstAtOrBelow(r, static_cast<size_t>(c), 0.1 * pk);
        if (d < 0) continue;
        fall.push_back(r.s[static_cast<size_t>(d)].tMs - r.s[static_cast<size_t>(c)].tMs);
    }
    printf("\n3. SPEED OF A FAST PULL (10%%->90%% of its own peak)\n");
    PrintSpan("press",   rise, "ms");
    PrintSpan("release", fall, "ms");
    puts("   (if these are only a frame or two, a quick stab crosses any threshold between two\n"
         "    reports; a delay window for 'quick pulls skip the soft stage' has to be longer than that)");

    // 4. Jitter ----------------------------------------------------------------
    double jitterRange = 0.0;
    printf("\n4. HELD HALF-PULL (finger tremor mid-range: sets a soft-stage hysteresis, not the full press's)\n");
    bool anyHalf = false;
    for (const auto& r : reps) {
        if (kStages[r.stage].kind != Kind::Half || r.s.empty()) continue;
        std::vector<double> settled;
        for (const auto& s : r.s)
            if (s.tMs - r.s.front().tMs >= 500.0) settled.push_back(s.val);
        if (settled.size() < 4) continue;
        const Stats hs = Compute(settled);
        jitterRange = Pct(hs.max - hs.min);
        anyHalf = true;
        printf("   held at %.1f%% (range %.1f%%..%.1f%%): wobble %.2f points peak-to-peak, sd %.2f\n",
               Pct(hs.mean), Pct(hs.min), Pct(hs.max), jitterRange, Pct(hs.sd));
    }
    if (!anyHalf) puts("   (no half-pull data)");

    // 5. Firmware full bit -----------------------------------------------------
    printf("\n5. FIRMWARE FULL-PULL BIT\n");
    // buf[4] is b[2], buf[5] is b[3] — b[] starts at buf[2].
    const int      fullByte = side == 1 ? 2 : 3;
    const uint8_t  fullMask = side == 1 ? SteamController::BTN_RT_FULL : SteamController::BTN_LT_FULL;
    const char*    fullName = side == 1 ? "BTN_RT_FULL (buf[4] bit 7)" : "BTN_LT_FULL (buf[5] bit 3)";
    double fwFireMin = -1.0, fwDropMed = -1.0;
    {
        std::vector<double> rises, falls;
        size_t framesSet = 0, framesAll = 0;
        for (const auto& r : reps) {
            bool prev = false;
            bool havePrev = false;
            for (const auto& s : r.s) {
                const bool bit = (s.b[fullByte] & fullMask) != 0;
                ++framesAll;
                if (bit) ++framesSet;
                if (havePrev && bit && !prev) rises.push_back(Pct(s.val));
                if (havePrev && !bit && prev) falls.push_back(Pct(s.val));
                prev = bit;
                havePrev = true;
            }
        }
        printf("   %s was set in %zu of %zu frames\n", fullName, framesSet, framesAll);
        PrintSpan("fires at", rises, "%");
        PrintSpan("drops at", falls, "%");
        if (!rises.empty()) fwFireMin = Compute(rises).min;
        if (!falls.empty()) fwDropMed = Compute(falls).med;
        if (framesSet == 0)
            puts("   -> never set: the bit is not a usable full-pull signal on this transport/firmware.");
    }

    // Bit scan: every button bit against "trigger fully pulled" vs "trigger up".
    if (!allPeaks.empty()) {
        const double highLevel = 0.9 * Percentile(allPeaks, 50.0);
        const double lowLevel  = 0.03 * kFull;
        size_t high = 0, low = 0;
        size_t hi[4][8] = {}, lo[4][8] = {};
        for (const auto& r : reps)
            for (const auto& s : r.s) {
                const bool isHigh = s.val >= highLevel;
                const bool isLow  = s.val <= lowLevel;
                if (isHigh) ++high;
                if (isLow)  ++low;
                for (int k = 0; k < 4; ++k)
                    for (int bit = 0; bit < 8; ++bit) {
                        const bool set = (s.b[k] >> bit) & 1;
                        if (set && isHigh) ++hi[k][bit];
                        if (set && isLow)  ++lo[k][bit];
                    }
            }
        printf("   bit scan (%zu fully-pulled frames vs %zu released frames):\n", high, low);
        bool found = false;
        if (high >= 5 && low >= 5) {
            for (int k = 0; k < 4; ++k)
                for (int bit = 0; bit < 8; ++bit) {
                    const double hf = static_cast<double>(hi[k][bit]) / static_cast<double>(high);
                    const double lf = static_cast<double>(lo[k][bit]) / static_cast<double>(low);
                    if (hf >= 0.9 && lf <= 0.02) {
                        printf("     buf[%d] bit %d (0x%02X): set in %.0f%% of full pulls, %.1f%% of rest%s\n",
                               2 + k, bit, 1 << bit, hf * 100.0, lf * 100.0,
                               (k == 2 && bit == 7) ? "   <- BTN_RT_FULL"
                             : (k == 3 && bit == 3) ? "   <- BTN_LT_FULL" : "");
                        found = true;
                    }
                }
        }
        if (!found) puts("     no bit tracks the full pull (nothing set in >=90% of full pulls and <=2% of rest)");
    }

    // Recommendation -----------------------------------------------------------
    printf("\nRECOMMENDATION\n");
    if (allPeaks.empty()) {
        puts("   need slow/fast/squeeze pulls to say anything — run the whole 'cal'.");
    } else {
        // Press: 95% of the weakest deliberate pull, capped at 95 so a trigger
        // that pegs at exactly 100 still has margin for a lighter grip.
        const double press = std::min(std::floor(Pct(minPeak) * 0.95), 95.0);
        // Release: follow the firmware's own measured drop point when there is
        // one — it is the click the user would otherwise get — and otherwise
        // leave several times the plateau's wobble. Never the half-pull wobble:
        // that is a finger hovering mid-range, and the full press is pinned
        // against the end of travel.
        double release;
        const char* releaseWhy;
        if (fwDropMed > 0.0 && fwDropMed < press) {
            release = std::floor(fwDropMed);
            releaseWhy = "the firmware bit's own median drop point";
        } else {
            release = std::max(press - std::max(15.0, std::ceil(plateauRange * 5.0)), 0.0);
            releaseWhy = "no firmware bit data; 15+ points below press";
        }
        printf("   full-press point  %.0f%%   (95%% of the weakest deliberate pull, %.1f%%)\n", press, Pct(minPeak));
        printf("   release point     %.0f%%   (%s)\n", release, releaseWhy);
        if (fwFireMin >= 0.0)
            printf("   firmware bit fired as low as %.1f%%, so the software point and the bit can disagree by %.1f points\n",
                   fwFireMin, std::fabs(press - fwFireMin));
        printf("   headroom above the noise ceiling: %.0f points\n", press - restCeil);
        printf("   soft stage (not part of v1): a threshold there needs at least %.0f points of hysteresis — the held half-pull wobbled that much\n",
               std::ceil(jitterRange));
        if (press - restCeil < 30.0)
            puts("   WARNING: little room above rest noise — the soft range would be tiny. Re-run and check the REST stage.");
        puts("   Try it: 'arm <candidate> <press> <release>' fires a haptic on exactly these crossings.");
    }
    puts("");
}

// ---------------------------------------------------------------------------
// Haptic commands
// ---------------------------------------------------------------------------

void PrintHapticTable() {
    puts("\nHaptic candidates:");
    for (int i = 1; i <= kCandidateCount; ++i) {
        auto it = g_ratings.find(i);
        char rating[16] = "";
        if (it != g_ratings.end()) snprintf(rating, sizeof(rating), "  [rated %d/5]", it->second);
        printf("  %d  %-15s %s%s\n", i, kCandidates[i].name, kCandidates[i].what, rating);
    }
    puts("");
}

void PrintLatency() {
    std::vector<double> lat;
    {
        std::lock_guard<std::mutex> lk(g_hMutex);
        lat = g_hLatencyMs;
    }
    if (lat.empty()) { puts("No haptic crossings recorded yet."); return; }
    const Stats s = Compute(lat);
    size_t late = 0;
    for (double x : lat) if (x > 20.0) ++late;
    printf("Crossing -> haptic report written: median %.1f ms  p95 %.1f ms  max %.1f ms  (n=%zu, %zu over 20 ms)\n",
           s.med, Percentile(lat, 95.0), s.max, s.n, late);
    puts("  (includes one report interval of detection delay; over ~30 ms a click stops feeling attached to the pull)");
}

void Live() {
    puts("Live view — pull the triggers. '|' marks the arm press point (90% unless armed). Any key to stop.");
    g_peak[0].store(0);
    g_peak[1].store(0);
    constexpr int kWidth = 24;
    auto bar = [&](int raw, int markRaw) {
        std::string b(static_cast<size_t>(kWidth), '.');
        const int fill = static_cast<int>(std::lround(Pct(raw) / 100.0 * kWidth));
        for (int i = 0; i < std::min(fill, kWidth); ++i) b[static_cast<size_t>(i)] = '#';
        const int mark = std::clamp(static_cast<int>(std::lround(Pct(markRaw) / 100.0 * kWidth)) - 1, 0, kWidth - 1);
        b[static_cast<size_t>(mark)] = '|';
        return b;
    };
    while (!_kbhit() && !g_quit.load()) {
        const int mark = g_armOn.load() ? g_armPress.load() : static_cast<int>(0.9 * kFull);
        printf("\rL [%s] %5.1f%% pk %5.1f %s  R [%s] %5.1f%% pk %5.1f %s ",
               bar(g_val[0].load(), mark).c_str(), Pct(g_val[0].load()), Pct(g_peak[0].load()),
               g_fullBit[0].load() ? "FULL" : "    ",
               bar(g_val[1].load(), mark).c_str(), Pct(g_val[1].load()), Pct(g_peak[1].load()),
               g_fullBit[1].load() ? "FULL" : "    ");
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    while (_kbhit()) _getch();
    printf("\nPeaks this view: L %.1f%%  R %.1f%%\n", Pct(g_peak[0].load()), Pct(g_peak[1].load()));
}

void Dump(const std::string& path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "w") != 0 || !f) { printf("Could not write %s\n", path.c_str()); return; }
    fprintf(f, "side,stage,rep,t_ms,val,b2,b3,b4,b5\n");
    std::lock_guard<std::mutex> lk(g_mutex);
    size_t rows = 0;
    for (int side = 0; side < 2; ++side) {
        int repNo = 0;
        for (const auto& r : g_reps[side]) {
            ++repNo;
            for (const auto& s : r.s) {
                fprintf(f, "%s,%s,%d,%.3f,%u,%u,%u,%u,%u\n", SideName(side), kStages[r.stage].name, repNo,
                        s.tMs, static_cast<unsigned>(s.val), static_cast<unsigned>(s.b[0]),
                        static_cast<unsigned>(s.b[1]), static_cast<unsigned>(s.b[2]),
                        static_cast<unsigned>(s.b[3]));
                ++rows;
            }
        }
    }
    fclose(f);
    printf("Wrote %zu samples to %s\n", rows, path.c_str());
}

std::vector<std::string> Split(const std::string& line) {
    std::istringstream is(line);
    std::vector<std::string> out;
    std::string t;
    while (is >> t) out.push_back(t);
    return out;
}

void PrintHelp() {
    puts(
        "\nTriggerProbe — what do the triggers report, and what feels like a click?\n"
        "\n"
        "  live                       bars for both triggers, with the press point marked\n"
        "  cal l | cal r              guided measurement of one trigger (about a minute)\n"
        "  stop                       abandon a calibration (data so far is kept)\n"
        "  sum [l|r]                  results + recommended press/release points\n"
        "  dump [file.csv]            write every recorded sample out for later analysis\n"
        "\n"
        "  hap                        list the haptic candidates\n"
        "  fire <n> [l|r|b]           fire candidate n once (default: both sides' own)\n"
        "  arm <n> [press%] [rel%]    fire candidate n when either trigger crosses press%\n"
        "                             (default 90) and again below rel% (default press-8)\n"
        "  arm <n> ... norel          ...on the way down only: skip the release haptic\n"
        "  disarm                     stop firing; prints crossing->write latency\n"
        "  rate <n> <1-5>             note how much candidate n felt like a click\n"
        "  status                     latency so far, ratings, report counters\n"
        "  q                          quit\n");
}

}  // namespace

int main(int argc, char** argv) {
    auto paths = SteamController::EnumerateAll();
    if (paths.empty()) {
        puts("No Steam Controller found (wired PID=1302, BT 1303 or dongle PID=1304).");
        return 1;
    }

    size_t which = 0;
    if (argc > 1) {
        which = static_cast<size_t>(std::strtoul(argv[1], nullptr, 10));
        if (which >= paths.size()) {
            printf("Interface %zu does not exist (%zu found).\n", which, paths.size());
            return 1;
        }
    }

    HidDevice dev;
    if (!dev.Open(paths[which])) {
        puts("Failed to open HID device.");
        return 1;
    }
    printf("Opened %ls\n", paths[which].c_str());
    if (paths.size() > 1 && argc <= 1) {
        printf("NOTE: %zu interfaces found; this is #0. If the triggers read zero, try\n"
               "      'TriggerProbe 1', 'TriggerProbe 2', ... (a puck has one per slot).\n",
               paths.size());
    }
    puts("Reading shared — lizard mode and Steam are untouched. The controller keeps its\n"
         "own mappings, so a trigger pull may also act on the desktop while you test.");

    PrintHelp();

    std::thread reader(ReaderThread, &dev);
    std::thread worker(HapticWorker, &dev);

    char lineBuf[256];
    while (true) {
        printf("> ");
        fflush(stdout);
        if (!fgets(lineBuf, sizeof(lineBuf), stdin)) break;

        const auto toks = Split(lineBuf);
        if (toks.empty()) continue;
        const std::string& cmd = toks[0];

        if (cmd == "q" || cmd == "quit") break;

        if (cmd == "?" || cmd == "help") { PrintHelp(); continue; }

        if (cmd == "live") { Live(); continue; }

        if (cmd == "cal") {
            if (toks.size() < 2 || (toks[1] != "l" && toks[1] != "r")) {
                puts("usage: cal l | cal r");
                continue;
            }
            const int side = toks[1] == "l" ? 0 : 1;
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                g_reps[side].clear();
                g_cal = Cal{};
                g_cal.active = true;
                g_cal.side = side;
            }
            printf("\nCalibrating the %s trigger. Use it the way you would while playing —\n"
                   "same finger, same grip. %d stages.\n", SideName(side), kStageCount);
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                PrintStagePrompt(0);
            }
            continue;
        }

        if (cmd == "stop") {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_cal.active = false;
            puts("Stopped. Data so far is kept — 'sum' to see it.");
            continue;
        }

        if (cmd == "sum") {
            if (toks.size() >= 2 && toks[1] == "l")      Summarize(0);
            else if (toks.size() >= 2 && toks[1] == "r") Summarize(1);
            else { Summarize(0); Summarize(1); }
            continue;
        }

        if (cmd == "dump") {
            Dump(toks.size() >= 2 ? toks[1] : "trigger_probe_samples.csv");
            continue;
        }

        if (cmd == "hap") { PrintHapticTable(); continue; }

        if (cmd == "fire") {
            const int n = toks.size() >= 2 ? std::atoi(toks[1].c_str()) : 0;
            if (n < 1 || n > kCandidateCount) { puts("usage: fire <1-7> [l|r|b]"); continue; }
            int side = 2;
            if (toks.size() >= 3) side = toks[2] == "l" ? 0 : (toks[2] == "r" ? 1 : 2);
            double t = 0.0;
            Fire(dev, n, side, t);
            printf("Fired %d (%s) on %s.\n", n, kCandidates[n].name,
                   side == 2 ? "both" : SideName(side));
            continue;
        }

        if (cmd == "arm") {
            const int n = toks.size() >= 2 ? std::atoi(toks[1].c_str()) : 0;
            if (n < 1 || n > kCandidateCount) { puts("usage: arm <1-7> [press%] [rel%] [norel]"); continue; }
            double press = 90.0;
            double rel = -1.0;
            bool onRelease = true;
            int numeric = 0;
            for (size_t i = 2; i < toks.size(); ++i) {
                if (toks[i] == "norel") { onRelease = false; continue; }
                const double v = std::atof(toks[i].c_str());
                if (numeric == 0) press = v; else rel = v;
                ++numeric;
            }
            press = std::clamp(press, 5.0, 100.0);
            if (rel < 0.0) rel = std::max(press - 8.0, 0.0);
            rel = std::clamp(rel, 0.0, press - 1.0);
            g_armCand.store(n);
            g_armPress.store(static_cast<int>(press / 100.0 * kFull));
            g_armRelease.store(static_cast<int>(rel / 100.0 * kFull));
            g_armOnRelease.store(onRelease);
            g_down[0].store(false);
            g_down[1].store(false);
            {
                std::lock_guard<std::mutex> lk(g_hMutex);
                g_hLatencyMs.clear();
            }
            g_armOn.store(true);
            printf("Armed: %s fires past %.0f%%%s. Pull a trigger. 'disarm' when done, 'rate %d <1-5>' to score it.\n",
                   kCandidates[n].name, press,
                   onRelease ? (std::string(" and again below ") + std::to_string(static_cast<int>(rel)) + "%").c_str()
                             : " (not on release)",
                   n);
            continue;
        }

        if (cmd == "disarm") {
            g_armOn.store(false);
            PrintLatency();
            continue;
        }

        if (cmd == "rate") {
            const int n = toks.size() >= 2 ? std::atoi(toks[1].c_str()) : 0;
            const int score = toks.size() >= 3 ? std::atoi(toks[2].c_str()) : 0;
            if (n < 1 || n > kCandidateCount || score < 1 || score > 5) {
                puts("usage: rate <1-7> <1-5>   (5 = feels most like a click)");
                continue;
            }
            g_ratings[n] = score;
            printf("Candidate %d (%s): %d/5\n", n, kCandidates[n].name, score);
            continue;
        }

        if (cmd == "status") {
            PrintLatency();
            PrintHapticTable();
            printf("Reports: %llu seen, %llu wrong id, %llu short\n",
                   static_cast<unsigned long long>(g_reportsSeen.load()),
                   static_cast<unsigned long long>(g_reportsWrongId.load()),
                   static_cast<unsigned long long>(g_reportsShort.load()));
            continue;
        }

        puts("Unknown command. 'help' lists them.");
    }

    g_armOn.store(false);
    g_quit.store(true);
    g_hCv.notify_all();
    reader.join();
    worker.join();
    SendRumble(dev, 0, 0);
    return 0;
}
