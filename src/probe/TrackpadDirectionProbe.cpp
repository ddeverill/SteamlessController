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
//
// LIVE MODE (--live [left]): opens the real controller (shared — run with
// SteamlessController CLOSED so nothing holds it exclusively and the firmware
// or Steam is generating its native haptics) and correlates our detection
// against the firmware's 0x40 haptic-fired notification. The firmware plays
// its own haptic at the moment IT judges a press/release happened, and tells
// the host with a 0x40 report — which makes the very haptics being felt a
// per-event ground truth: each 0x40 is timestamped against our area-based
// press detection, the firmware click bit, and direction changes, and the
// exit summary gives min/median/max deltas. Detection uses the same detector
// the app ships (kPadPressArea/kPadPressFrames), so what this measures is
// what users will feel.

#include "app/TrackpadInput.h"
#include "hid/HidDevice.h"
#include "steam/SteamController.h"
#include <algorithm>
#include <atomic>
#include <conio.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

// ---------------------------------------------------------------------------
// Live correlation mode
// ---------------------------------------------------------------------------

// A 0x40 this long after one of our events is not an answer to it.
constexpr double kCorrelateWindowMs = 300.0;

double NowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / freq;
}

// Mirrors Slot::PressState in ControllerManager.cpp — the probe must judge
// presses with exactly the detector the app ships, or the correlation
// validates nothing. Keep in lockstep.
struct PressState {
    bool pressed = false;
    int  above   = 0;
    int  below   = 0;

    bool Update(uint16_t area, bool clickBit) {
        const bool raw = area >= kPadPressArea || clickBit;
        if (raw) { ++above; below = 0; } else { ++below; above = 0; }
        if (!pressed && above >= kPadPressFrames)      pressed = true;
        else if (pressed && below >= kPadPressFrames)  pressed = false;
        return pressed;
    }
};

// When the firmware's haptic LED the event we only just noticed, say by how
// much — this is the direction the deltas point when our detection is slow.
std::string AfterHaptic(double now, double lastHapticAt) {
    const double d = now - lastHapticAt;
    if (d < 0 || d > kCorrelateWindowMs) return "";
    char b[48];
    snprintf(b, sizeof(b), "  (+%.1fms after HAPTIC)", d);
    return b;
}

void PrintStats(const char* name, std::vector<double>& v) {
    if (v.empty()) {
        printf("  %s -> haptic: no correlated pairs\n", name);
        return;
    }
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double d : v) sum += d;
    printf("  %s -> haptic: n=%zu  min=%.1fms  median=%.1fms  avg=%.1fms  max=%.1fms\n",
           name, v.size(), v.front(), v[v.size() / 2],
           sum / static_cast<double>(v.size()), v.back());
}

// A report seen on one of the controller's other HID collections.
struct SideReport {
    double  t    = 0;
    uint8_t id   = 0;
    size_t  len  = 0;
    uint8_t bytes[24] = {};
    int     src  = 0;
};

std::mutex              g_sideMutex;
std::vector<SideReport> g_sideReports;
std::atomic<bool>       g_watching{false};
std::atomic<bool>       g_stop{false};

// Ctrl+C has to reach the summary rather than kill the process: the numbers
// this probe exists to produce are all printed at the end, and losing them
// means running the whole session again.
BOOL WINAPI ConsoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}

// Input reports are routed to HID collections by report id, so a notification
// declared in the keyboard or mouse collection never reaches a handle on the
// vendor collection — which is why the first version of this correlation saw
// nothing. The USB capture that recorded 0x40 was a bus-level trace and saw
// every collection at once; this is the equivalent from user mode.
void WatchCollection(HidDevice* dev, int src) {
    while (g_watching.load()) {
        uint8_t buf[64] = {};
        const size_t n = dev->ReadInputReport(buf, sizeof(buf), 50);
        if (n == 0) continue;
        SideReport e;
        e.t   = NowMs();
        e.id  = buf[0];
        e.len = n;
        std::memcpy(e.bytes, buf, n < sizeof(e.bytes) ? n : sizeof(e.bytes));
        e.src = src;
        std::lock_guard<std::mutex> lk(g_sideMutex);
        g_sideReports.push_back(e);
    }
}

// One PRESS..RELEASE span as our detector saw it, with the peak contact area
// reached and whether the firmware ever agreed it was a click. The two
// populations that come out of this are what a press threshold has to separate.
struct Span {
    double   startedAt   = 0;
    double   durationMs  = 0;
    uint16_t peakArea    = 0;
    bool     sawClickBit = false;
};

void PrintAreaStats(const char* name, std::vector<uint16_t> v) {
    if (v.empty()) {
        printf("  %-28s none\n", name);
        return;
    }
    std::sort(v.begin(), v.end());
    printf("  %-28s n=%-3zu peak area  min=%u  median=%u  max=%u\n",
           name, v.size(), v.front(), v[v.size() / 2], v.back());
}

int RunLive(bool leftPad) {
    auto paths = SteamController::EnumerateAll();
    if (paths.empty()) {
        puts("No Steam Controller found (wired or dongle).");
        return 1;
    }

    HidDevice dev;
    if (!dev.Open(paths[0])) {
        puts("Failed to open the controller. If SteamlessController is running,"
             " close it first - its exclusive claim locks this probe out.");
        return 1;
    }
    printf("Opened %ls\n", paths[0].c_str());

    // Watch every other collection this controller exposes, so a haptic
    // notification carried on one of them is not invisible here.
    std::vector<std::wstring> allPaths;
    for (uint16_t pid : { SteamController::SC2026_PID, SteamController::SC2026_DONGLE_PID,
                          SteamController::SC2026_NEREID_PID })
        for (auto& p : HidDevice::Enumerate(SteamController::VALVE_VID, pid, /*usagePage=*/0))
            allPaths.push_back(p);

    std::vector<std::unique_ptr<HidDevice>> sideDevs;
    std::vector<std::wstring>               sideNames;
    std::vector<std::thread>                watchers;
    puts("\nOther HID collections on this controller:");
    for (const auto& p : allPaths) {
        if (p == paths[0]) continue;
        auto d = std::make_unique<HidDevice>();
        if (!d->Open(p)) {
            // Windows keeps system keyboard/mouse collections open exclusively,
            // so this is expected for some of them rather than a fault.
            printf("  [skip] %ls (error %lu)\n", p.c_str(), GetLastError());
            continue;
        }
        printf("  [watch #%zu] %ls\n", sideDevs.size(), p.c_str());
        sideNames.push_back(p);
        sideDevs.push_back(std::move(d));
    }
    g_watching = true;
    for (size_t i = 0; i < sideDevs.size(); ++i)
        watchers.emplace_back(WatchCollection, sideDevs[i].get(), static_cast<int>(i));

    printf("\nCorrelating %s-pad detection against firmware haptic notifications.\n",
           leftPad ? "left" : "right");
    puts("Native (firmware/Steam) haptics must be active: keep SteamlessController closed.");
    puts("Press q to quit and print the summary.\n");

    TrackpadInput pad;
    pad.SetPad(leftPad);
    pad.SetMode(TrackpadMode::DirectionalPad);

    PressState press;
    bool    prevPressed  = false;
    bool    prevClickBit = false;
    uint8_t prevDirs     = DirNone;

    const double t0 = NowMs();
    double lastPressAt     = -1e9, lastReleaseAt = -1e9;
    double lastClickDownAt = -1e9, lastClickUpAt = -1e9;
    double lastDirAt       = -1e9, lastHapticAt  = -1e9;

    std::vector<double> pressToHaptic;
    std::vector<double> releaseToHaptic;
    int presses = 0, haptics = 0;
    int otherIds[256] = {};

    std::vector<Span> spans;
    Span              span;
    int               sideIds[256] = {};

    // The area profile around real clicks. The firmware's click bit is the only
    // ground truth available for "this was a press", and these two numbers are
    // what any press threshold has to live between:
    //
    //   clickDips  - the lowest area reached WHILE a click was held. A release
    //                threshold above this splits one press into several, which
    //                is the failure kPadPressArea's comment was written about.
    //   gapFloors  - the lowest area reached BETWEEN two consecutive clicks. A
    //                release threshold below this merges them into one press,
    //                losing the second click entirely.
    //
    // A workable threshold exists only if every gap floor is below every dip.
    std::vector<uint16_t> clickDips, gapFloors;
    uint16_t minDuringClick   = 0xFFFF;
    uint16_t minBetweenClicks = 0xFFFF;
    bool     inGap            = false;

    // Trace of a sustained hold, which is the case neither the click bit nor
    // the numbers above can describe. clickDips is bounded from below by the
    // firmware's own release threshold — the bit goes false the moment the
    // area falls through it — so it reports where the FIRMWARE gave up, not
    // how far a thumb that is still deliberately holding actually falls. A
    // release threshold has to survive that second number, so measure it
    // directly: while there is real contact, summarise the area every 100ms.
    static constexpr uint16_t kTraceFloor = 500;
    bool     traceOpen  = false;
    double   traceStart = 0;
    uint16_t traceMin   = 0;
    uint16_t traceMax   = 0;
    bool     traceClick = false;

    auto stamp = [&](double t) { printf("%9.1f  ", t - t0); };

    // Anything arriving on another collection, printed in the same timeline so
    // it can be read against our edges.
    auto drainSideReports = [&] {
        std::vector<SideReport> batch;
        {
            std::lock_guard<std::mutex> lk(g_sideMutex);
            batch.swap(g_sideReports);
        }
        for (const auto& e : batch) {
            ++sideIds[e.id];
            if (sideIds[e.id] > 6) continue;  // enough to identify it
            stamp(e.t);
            printf("SIDE #%d id=0x%02X (%zu bytes) [", e.src, e.id, e.len);
            for (size_t i = 0; i < e.len && i < 12; ++i)
                printf("%s%02X", i ? " " : "", e.bytes[i]);
            printf("]");
            const double d = e.t - lastPressAt;
            if (d >= 0 && d <= kCorrelateWindowMs) printf("  +%.1fms after PRESS", d);
            printf("\n");
        }
    };

    SetConsoleCtrlHandler(ConsoleCtrl, TRUE);

    while (!g_stop.load()) {
        if (_kbhit() && (_getch() | 0x20) == 'q') break;
        drainSideReports();

        uint8_t buf[64] = {};
        const size_t n = dev.ReadInputReport(buf, sizeof(buf), /*timeoutMs=*/50);
        if (n == 0) continue;
        const double now = NowMs();

        if (!SteamController::IsStateReportId(buf[0])) {
            if (buf[0] == 0x40) {
                ++haptics;
                lastHapticAt = now;
                stamp(now);
                printf("HAPTIC 0x40 [");
                for (size_t i = 0; i < n && i < 16; ++i)
                    printf("%s%02X", i ? " " : "", buf[i]);
                printf("]");

                struct Ref { const char* name; double at; };
                const Ref refs[] = {
                    { "PRESS",         lastPressAt     },
                    { "RELEASE",       lastReleaseAt   },
                    { "clickbit-down", lastClickDownAt },
                    { "clickbit-up",   lastClickUpAt   },
                    { "dir-change",    lastDirAt       },
                };
                for (const auto& r : refs) {
                    const double d = now - r.at;
                    if (d >= 0 && d <= kCorrelateWindowMs)
                        printf("  +%.1fms after %s", d, r.name);
                }
                printf("\n");

                // One pair per haptic, attributed to whichever of our edges is
                // live: a press if one is being held, else the release.
                if (lastPressAt > lastReleaseAt && now - lastPressAt <= kCorrelateWindowMs)
                    pressToHaptic.push_back(now - lastPressAt);
                else if (now - lastReleaseAt <= kCorrelateWindowMs)
                    releaseToHaptic.push_back(now - lastReleaseAt);
            } else if (buf[0] != SteamController::REPORT_BATTERY_STATUS) {
                // The 0x40 id came from one capture session; anything else
                // unexpected in the stream is worth a look in case the haptic
                // notification is richer than we thought.
                if (otherIds[buf[0]]++ < 5) {
                    stamp(now);
                    printf("OTHER 0x%02X (%zu bytes) [", buf[0], n);
                    for (size_t i = 0; i < n && i < 16; ++i)
                        printf("%s%02X", i ? " " : "", buf[i]);
                    printf("]\n");
                }
            }
            continue;
        }

        if (n < 30) continue;

        const uint8_t flags    = leftPad ? buf[5] : buf[4];
        const bool    clickBit = (flags & (leftPad ? SteamController::BTN_TP_LT_CLICK
                                                   : SteamController::BTN_TP_RT_CLICK)) != 0;
        uint16_t area = 0;
        std::memcpy(&area, buf + (leftPad ? 22 : 28), 2);

        const bool pressed = press.Update(area, clickBit);
        pad.Update(buf, n, pressed);

        if (pressed && area > span.peakArea) span.peakArea = area;
        if (pressed && clickBit)             span.sawClickBit = true;

        // Profile the area against the firmware's own verdict, frame by frame.
        const bool touching = (flags & (leftPad ? SteamController::BTN_TP_LT
                                                : SteamController::BTN_TP_RT)) != 0;

        if (area >= kTraceFloor) {
            if (!traceOpen) {
                traceOpen  = true;
                traceStart = now;
                traceMin   = traceMax = area;
                traceClick = clickBit;
            } else {
                if (area < traceMin) traceMin = area;
                if (area > traceMax) traceMax = area;
                traceClick = traceClick || clickBit;
            }
            if (now - traceStart >= 100.0) {
                stamp(now);
                printf("HOLD    area %u..%u%s\n", traceMin, traceMax,
                       traceClick ? "   clickbit" : "");
                traceOpen = false;
            }
        } else if (traceOpen) {
            stamp(now);
            printf("HOLD    area %u..%u%s   (contact ended)\n", traceMin, traceMax,
                   traceClick ? "   clickbit" : "");
            traceOpen = false;
        }
        if (clickBit) {
            if (area < minDuringClick) minDuringClick = area;
            if (inGap) {
                gapFloors.push_back(minBetweenClicks);
                inGap = false;
            }
        } else {
            if (prevClickBit) {  // the click just ended
                if (minDuringClick != 0xFFFF) clickDips.push_back(minDuringClick);
                minDuringClick   = 0xFFFF;
                minBetweenClicks = 0xFFFF;
                inGap            = true;
            }
            // Only while the thumb stays down: a gap where the finger lifts
            // separates trivially and says nothing about the threshold.
            if (inGap && touching && area < minBetweenClicks) minBetweenClicks = area;
            if (inGap && !touching) inGap = false;
        }

        if (clickBit != prevClickBit) {
            (clickBit ? lastClickDownAt : lastClickUpAt) = now;
            stamp(now);
            printf("clickbit %-4s (area=%u)%s\n", clickBit ? "down" : "up", area,
                   AfterHaptic(now, lastHapticAt).c_str());
            prevClickBit = clickBit;
        }
        if (pressed != prevPressed) {
            if (pressed) {
                ++presses;
                lastPressAt = now;
                span = Span{};
                span.startedAt = now;
                span.peakArea  = area;
            } else {
                lastReleaseAt   = now;
                span.durationMs = now - span.startedAt;
                spans.push_back(span);
            }
            stamp(now);
            printf("%-7s (area=%u)%s\n", pressed ? "PRESS" : "RELEASE", area,
                   AfterHaptic(now, lastHapticAt).c_str());
            prevPressed = pressed;
        }
        const uint8_t dirs = pad.Directions();
        if (dirs != prevDirs) {
            lastDirAt = now;
            stamp(now);
            printf("dirs     %s -> %s\n",
                   DirsName(prevDirs).c_str(), DirsName(dirs).c_str());
            prevDirs = dirs;
        }
    }

    g_watching = false;
    for (auto& t : watchers) t.join();

    printf("\nSummary: %d press(es) detected, %d haptic notification(s) on the"
           " vendor collection\n", presses, haptics);
    PrintStats("PRESS", pressToHaptic);
    PrintStats("RELEASE", releaseToHaptic);

    for (int id = 0; id < 256; ++id)
        if (otherIds[id])
            printf("  vendor collection, unexpected id 0x%02X: %d time(s)\n", id, otherIds[id]);
    for (int id = 0; id < 256; ++id)
        if (sideIds[id])
            printf("  other collections, id 0x%02X: %d time(s)\n", id, sideIds[id]);
    if (sideDevs.empty())
        puts("  no other collections could be opened — a notification carried on"
             " one of them would be invisible here");

    // What our detector called a press, split by whether the firmware agreed.
    // A press threshold has to sit between these two populations; if they
    // overlap, contact area alone cannot separate them.
    std::vector<uint16_t> withClick, withoutClick;
    double withoutDurSum = 0;
    for (const auto& s : spans) {
        (s.sawClickBit ? withClick : withoutClick).push_back(s.peakArea);
        if (!s.sawClickBit) withoutDurSum += s.durationMs;
    }
    printf("\nDetected spans, by whether the firmware's click bit ever agreed"
           " (kPadPressArea=%d):\n", kPadPressArea);
    PrintAreaStats("firmware agreed (press)", withClick);
    PrintAreaStats("firmware never agreed", withoutClick);
    if (!withoutClick.empty())
        printf("  %zu span(s) the firmware never called a click, mean duration %.0fms\n",
               withoutClick.size(), withoutDurSum / static_cast<double>(withoutClick.size()));
    if (!withClick.empty() && !withoutClick.empty()) {
        auto lo = *std::min_element(withClick.begin(), withClick.end());
        auto hi = *std::max_element(withoutClick.begin(), withoutClick.end());
        if (hi < lo)
            printf("  the two populations are separable: any threshold in %u..%u"
                   " splits them\n", hi + 1, lo);
        else
            printf("  the populations OVERLAP (%u..%u) - contact area alone cannot"
                   " split these\n", lo, hi);
    }

    // The threshold design, straight off the firmware's own verdict.
    printf("\nArea profile around real clicks (firmware click bit as truth):\n");
    PrintAreaStats("area when the bit dropped", clickDips);
    PrintAreaStats("floor between two clicks", gapFloors);
    puts("  NOTE: the first row is the firmware's own release level, not how far"
         " a still-held thumb falls - the bit goes false the moment the area"
         " crosses it. Read the HOLD lines for that.");
    if (!clickDips.empty() && !gapFloors.empty()) {
        const uint16_t worstDip = *std::min_element(clickDips.begin(), clickDips.end());
        const uint16_t worstGap = *std::max_element(gapFloors.begin(), gapFloors.end());
        printf("  firmware releases around %u; consecutive clicks separated down"
               " to %u\n", worstDip, worstGap);
    }
    return 0;
}

}  // namespace

int RunChecks() {
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

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--live") == 0) {
            const bool leftPad = (i + 1 < argc) && _stricmp(argv[i + 1], "left") == 0;
            return RunLive(leftPad);
        }
    }
    return RunChecks();
}
