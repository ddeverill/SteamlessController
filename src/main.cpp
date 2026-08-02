#include <Windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>
#include "hid/HidDevice.h"
#include "steam/SteamController.h"

static SteamController* g_controller = nullptr;

static void OnSignal(int) {
    printf("\nCleaning up — re-enabling lizard mode...\n");
    if (g_controller) {
        g_controller->EnableLizardMode();
        g_controller->Close();
    }
    exit(0);
}

// ---------------------------------------------------------------------------
// Transport identification — shared with the app (path markers + PID tokens;
// wired HID paths don't contain "usb", so a plain substring test mislabels them).
// ---------------------------------------------------------------------------

static const char* GuessTransport(const std::wstring& rawPath) {
    return SteamController::TransportName(SteamController::TransportFromPath(rawPath));
}

// ---------------------------------------------------------------------------
// Enumeration — dump every Valve HID interface with full detail so a new
// transport (Bluetooth) is visible even before we know its PID.
// ---------------------------------------------------------------------------

struct IfaceInfo {
    std::wstring path;
    uint16_t     pid       = 0;
    uint16_t     usagePage = 0;
    uint16_t     usage     = 0;
};

static std::vector<IfaceInfo> EnumerateAllValveDevices() {
    printf("=== All Valve HID interfaces (VID=28DE, any PID) ===\n");
    auto paths = HidDevice::Enumerate(SteamController::VALVE_VID, 0);
    std::vector<IfaceInfo> out;
    if (paths.empty()) {
        printf("  None found. Plug in the controller/dongle, or pair it in\n"
               "  Windows Bluetooth settings, then re-run.\n\n");
        return out;
    }

    for (auto const& path : paths) {
        IfaceInfo info;
        info.path = path;

        HANDLE h = CreateFileW(path.c_str(), 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            printf("  [%zu] (open failed, error %lu)  %s\n      path: %ls\n",
                   out.size(), GetLastError(), GuessTransport(path), path.c_str());
            out.push_back(std::move(info));
            continue;
        }

        HIDD_ATTRIBUTES attrs{};
        attrs.Size = sizeof(attrs);
        HidD_GetAttributes(h, &attrs);
        info.pid = attrs.ProductID;

        wchar_t productBuf[128] = L"(unknown)";
        HidD_GetProductString(h, productBuf, sizeof(productBuf));
        wchar_t serialBuf[128] = L"(none)";
        HidD_GetSerialNumberString(h, serialBuf, sizeof(serialBuf));

        USHORT inLen = 0, outLen = 0, featLen = 0;
        PHIDP_PREPARSED_DATA preparsed;
        if (HidD_GetPreparsedData(h, &preparsed)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
                info.usagePage = caps.UsagePage;
                info.usage     = caps.Usage;
                inLen   = caps.InputReportByteLength;
                outLen  = caps.OutputReportByteLength;
                featLen = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(preparsed);
        }

        CloseHandle(h);

        // Can a write-exclusive handle be had? A "no" here is exactly why game
        // mode used to refuse a perfectly good controller, so it is worth
        // seeing per interface instead of inferring it from a later failure.
        HANDLE exclusive = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_OVERLAPPED, nullptr);
        const DWORD exclusiveErr =
            exclusive == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (exclusive != INVALID_HANDLE_VALUE) CloseHandle(exclusive);

        // Which slot is actually live? The puck publishes four controller
        // interfaces and only an occupied one emits state reports.
        const bool controllerIface =
            info.usagePage == SteamController::VENDOR_USAGE_PAGE
            && info.usage == SteamController::CONTROLLER_USAGE;
        size_t  reportLen = 0;
        uint8_t reportId  = 0;
        if (controllerIface) {
            HidDevice probe;
            if (probe.Open(path)) {
                uint8_t report[64]{};
                reportLen = probe.ReadInputReport(report, sizeof(report), 250);
                if (reportLen > 0) reportId = report[0];
            }
        }

        const bool vendor = (info.usagePage == SteamController::VENDOR_USAGE_PAGE);
        printf("  [%zu]%s PID=%04X  Usage=%04X:%04X  In=%u Out=%u Feat=%u  fw=0x%04X  %s\n"
               "      \"%ls\"  serial=%ls\n"
               "      exclusive=%s",
               out.size(), vendor ? "*" : " ", attrs.ProductID,
               info.usagePage, info.usage, inLen, outLen, featLen,
               attrs.VersionNumber, GuessTransport(path),
               productBuf, serialBuf,
               exclusiveErr == ERROR_SUCCESS ? "yes" : "no");
        if (exclusiveErr != ERROR_SUCCESS)
            printf(" (error %lu)", exclusiveErr);
        if (reportLen > 0)
            printf("  live=0x%02X/%zu bytes", reportId, reportLen);
        else if (controllerIface)
            printf("  live=none (empty slot?)");
        printf("\n      path: %ls\n", path.c_str());

        out.push_back(std::move(info));
    }
    printf("  (* = vendor collection 0x%04X — the game-input interface)\n\n",
           SteamController::VENDOR_USAGE_PAGE);
    return out;
}

// ---------------------------------------------------------------------------
// Calibration — collect idle frames to identify analog axis byte positions
// ---------------------------------------------------------------------------

static constexpr int     CALIB_FRAMES   = 200;
static constexpr uint8_t AXIS_THRESHOLD = 0;   // any idle movement → treat as axis

// Force specific byte pairs through calibration suppression even if they move at idle.
// Set to true to pin those bytes as "not an axis" so they appear in diff output.
static constexpr bool FORCE_SHOW_BYTES_10_11 = false;  // left joystick X
static constexpr bool FORCE_SHOW_BYTES_12_13 = false;  // left joystick Y
static constexpr bool FORCE_SHOW_BYTES_14_15 = false;  // right joystick Y
static constexpr bool FORCE_SHOW_BYTES_16_17 = false;  // right joystick Y

struct ByteStats {
    uint8_t lo  = 0xFF;
    uint8_t hi  = 0x00;
    bool    seeded = false;
    uint8_t range() const { return seeded ? (hi - lo) : 0; }
    void feed(uint8_t v) {
        if (!seeded) { lo = hi = v; seeded = true; return; }
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
};

static ByteStats s_stats[64];
static bool      s_isAxis[64] = {};   // true → suppress in diff output
static size_t    s_reportLen  = 0;
static uint8_t   s_stateId    = 0;    // which state report ID this transport uses

// Returns false when no state reports arrived in time (e.g. a transport that
// frames input differently) — the caller falls back to a raw dump.
static bool RunCalibration(SteamController& ctrl) {
    printf("--- Calibration: leave controller idle, collecting %d frames ---\n",
           CALIB_FRAMES);

    const auto start    = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(10);

    uint8_t buf[64];
    int collected = 0;
    while (collected < CALIB_FRAMES) {
        if (std::chrono::steady_clock::now() > deadline) {
            printf("  TIMEOUT: no state reports (0x42/0x45) after 10s — %d collected.\n"
                   "  Falling back to raw report dump.\n\n", collected);
            return false;
        }
        size_t n = ctrl.ReadReport(buf, sizeof(buf), 200);
        if (n == 0 || !SteamController::IsStateReportId(buf[0]))
            continue;
        if (s_stateId == 0) {
            s_stateId = buf[0];
            printf("  State report ID: 0x%02X, %zu bytes\n", s_stateId, n);
        }
        if (s_reportLen == 0) s_reportLen = n;
        for (size_t i = 1; i < n; ++i)
            s_stats[i].feed(buf[i]);
        ++collected;
        if (collected % 50 == 0)
            printf("  %d / %d\n", collected, CALIB_FRAMES);
    }

    const double secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    printf("  Measured state-report rate: ~%.0f Hz\n", collected / secs);

    // Apply force-show overrides before marking axes.
    auto forceShow = [](size_t i) -> bool {
        if constexpr (FORCE_SHOW_BYTES_10_11) {
            if (i == 10 || i == 11) return true;
        }
        if constexpr (FORCE_SHOW_BYTES_12_13) {
            if (i == 12 || i == 13) return true;
        }
        if constexpr (FORCE_SHOW_BYTES_14_15) {
            if (i == 14 || i == 15) return true;
        }
        if constexpr (FORCE_SHOW_BYTES_16_17) {
            if (i == 16 || i == 17) return true;
        }
        return false;
    };

    printf("\n--- Calibration complete. Byte ranges (idle): ---\n");
    printf("  [idx]  lo   hi  range  class\n");
    for (size_t i = 1; i < s_reportLen; ++i) {
        const char* cls = "";
        if (forceShow(i)) {
            s_isAxis[i] = false;
            cls = "  [forced]";
        } else if (s_stats[i].range() > AXIS_THRESHOLD) {
            s_isAxis[i] = true;
            cls = "  <axis>";
        } else if (s_stats[i].hi == 0) {
            cls = "  (zero)";
        } else {
            cls = "  [candidate]";
        }
        printf("  [%02zu]  %02X   %02X   %3u   %s\n",
               i, s_stats[i].lo, s_stats[i].hi, s_stats[i].range(), cls);
    }
    printf("\nAxes suppressed. Press buttons one at a time — only stable bytes shown.\n\n");
    return true;
}

// ---------------------------------------------------------------------------
// Differential display — axes masked, only changes shown
// ---------------------------------------------------------------------------

static uint8_t s_prevState[64];
static bool    s_hasPrevState = false;

static void PrintStateDiff(const uint8_t* buf, size_t n) {
    if (n < 2) return;

    bool anyDiff = false;
    for (size_t i = 1; i < n; ++i) {
        if (s_isAxis[i]) continue;
        bool changed = !s_hasPrevState || (buf[i] != s_prevState[i]);
        if (changed) {
            if (!anyDiff) { printf("  [idx]  new  old\n"); anyDiff = true; }
            printf("  [%02zu]   %02X  (%02X)\n", i, buf[i], s_hasPrevState ? s_prevState[i] : 0);
        }
    }

    memcpy(s_prevState, buf, n);
    s_hasPrevState = true;
}

static void PrintRawReport(const uint8_t* buf, size_t n) {
    printf("[0x%02X %zu bytes] ", buf[0], n);
    for (size_t i = 0; i < n; ++i) printf("%02X ", buf[i]);
    printf("\n");
}

// ---------------------------------------------------------------------------
// IMU test mode (--imu) — enable raw accel+gyro and print parsed samples.
// Answers whether the IMU streams on this transport: a live IMU shows an
// incrementing timestamp, gravity (~±0x4000 region) on one accel axis, and
// gyro spikes when the controller rotates. Frozen constants = not streaming.
// ---------------------------------------------------------------------------

static void RunImuTest(SteamController& controller, bool lizardOff) {
    printf("--- IMU test: enabling raw accel + gyro ---\n");
    if (!controller.SetImuEnabled(true))
        printf("SetImuEnabled FAILED — watching reports anyway.\n");
    printf("Rotate and tilt the controller. Printing ~3 samples/sec; Ctrl+C to quit.\n\n");

    uint8_t buf[64];
    size_t  lastLen          = 0;
    auto    lastPrint        = std::chrono::steady_clock::now();
    auto    lastKeepalive    = std::chrono::steady_clock::now();

    while (true) {
        if (lizardOff) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastKeepalive >= std::chrono::seconds(2)) {
                lastKeepalive = now;
                controller.SendKeepalive();
            }
        }

        size_t n = controller.ReadReport(buf, sizeof(buf), 100);
        if (n == 0) continue;
        if (!SteamController::IsStateReportId(buf[0])) {
            PrintRawReport(buf, n);
            continue;
        }
        if (n != lastLen) {
            printf("state report: id=0x%02X length=%zu bytes\n", buf[0], n);
            lastLen = n;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastPrint < std::chrono::milliseconds(300)) continue;
        lastPrint = now;

        if (n < 46) {
            printf("(report too short for IMU data: %zu bytes)\n", n);
            continue;
        }
        uint32_t ts;
        int16_t  ax, ay, az, gx, gy, gz;
        memcpy(&ts, buf + 30, 4);
        memcpy(&ax, buf + 34, 2);
        memcpy(&ay, buf + 36, 2);
        memcpy(&az, buf + 38, 2);
        memcpy(&gx, buf + 40, 2);
        memcpy(&gy, buf + 42, 2);
        memcpy(&gz, buf + 44, 2);
        printf("ts=%10u  accel=(%6d, %6d, %6d)  gyro=(%6d, %6d, %6d)\n",
               ts, ax, ay, az, gx, gy, gz);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static size_t PromptIndex(size_t maxIdx, size_t defaultIdx) {
    printf("Select interface [0-%zu] (Enter = %zu): ", maxIdx, defaultIdx);
    char line[32];
    if (!fgets(line, sizeof(line), stdin)) return defaultIdx;
    if (line[0] == '\n' || line[0] == '\0') return defaultIdx;
    char* end = nullptr;
    long v = strtol(line, &end, 10);
    if (end == line || v < 0 || static_cast<size_t>(v) > maxIdx) return defaultIdx;
    return static_cast<size_t>(v);
}

int main(int argc, char** argv) {
    signal(SIGINT,  OnSignal);
    signal(SIGTERM, OnSignal);

    // --enumerate-only is accepted as an alias so instructions written against
    // the puck investigation keep working.
    const bool enumOnly = argc > 1 && (strcmp(argv[1], "--enum") == 0
                                    || strcmp(argv[1], "--enumerate-only") == 0);
    const bool imuMode  = (argc > 1 && strcmp(argv[1], "--imu")  == 0);

    printf("=== SteamProbe — 2026 Steam Controller discovery tool ===\n\n");
    printf("NOTE: Close Steam before running this tool.\n\n");

    auto ifaces = EnumerateAllValveDevices();
    if (enumOnly) return ifaces.empty() ? 1 : 0;
    if (ifaces.empty()) return 1;

    // Default to the first vendor-page interface; anything is selectable so a
    // Bluetooth transport that frames things differently can still be probed.
    size_t defaultIdx = 0;
    for (size_t i = 0; i < ifaces.size(); ++i) {
        if (ifaces[i].usagePage == SteamController::VENDOR_USAGE_PAGE) {
            defaultIdx = i;
            break;
        }
    }
    size_t chosen = ifaces.size() > 1
        ? PromptIndex(ifaces.size() - 1, defaultIdx)
        : defaultIdx;
    printf("Opening [%zu] (%s)...\n\n", chosen, GuessTransport(ifaces[chosen].path));

    if (argc > 1 && strcmp(argv[1], "--enumerate-only") == 0)
        return 0;

    SteamController controller;
    g_controller = &controller;

    if (!controller.Open(ifaces[chosen].path)) {
        fprintf(stderr, "Could not open device.\n");
        return 1;
    }

    // Feature-report command channel — the critical unknown on a new
    // transport. Failure here is a finding, not a fatal error: input reports
    // may still flow, so keep going and dump whatever arrives.
    printf("Disabling lizard mode (feature-report command channel test)...\n");
    const bool lizardOff = controller.DisableLizardMode();
    if (lizardOff) {
        printf("Lizard mode OFF — feature reports work on this transport.\n\n");
    } else {
        printf("*** Lizard-off FAILED — the feature-report command channel may not\n"
               "*** work on this transport. Continuing with raw input dump.\n\n");
    }

    if (imuMode) {
        RunImuTest(controller, lizardOff);
        return 0;
    }

    // Output-report test — independent of the feature channel.
    printf("--- Output-report test: haptic tick each pad, then a rumble pulse ---\n");
    controller.PulseTrackpadHaptic(/*left=*/false, /*strongClick=*/true);
    Sleep(150);
    controller.PulseTrackpadHaptic(/*left=*/true, /*strongClick=*/true);
    Sleep(150);
    controller.SetRumble(0x90, 0x90);
    Sleep(300);
    controller.SetRumble(0, 0);
    printf("(any 'WriteOutputReport ... failed' lines above mean output reports\n"
           " were rejected; silence + felt haptics = output path works)\n\n");

    const bool calibrated = RunCalibration(controller);
    if (!calibrated)
        printf("Raw dump mode: state reports shown 1-in-25, all others in full.\n\n");

    uint8_t  buf[64];
    uint32_t stateCount       = 0;
    auto     lastKeepalive    = std::chrono::steady_clock::now();
    bool     keepaliveFailing = false;

    while (true) {
        // Keepalive — same cadence as the tray app, so the firmware doesn't
        // silently revert to lizard mode mid-session. Also a live health
        // check of the feature channel: watch for FAIL/recover transitions.
        if (lizardOff) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastKeepalive >= std::chrono::seconds(2)) {
                lastKeepalive = now;
                const bool ok = controller.SendKeepalive();
                if (!ok && !keepaliveFailing) {
                    keepaliveFailing = true;
                    printf("*** KEEPALIVE failed — feature channel dropped (sleep? out of range?)\n");
                } else if (ok && keepaliveFailing) {
                    keepaliveFailing = false;
                    printf("*** KEEPALIVE recovered\n");
                }
            }
        }

        size_t n = controller.ReadReport(buf, sizeof(buf), 100);
        if (n == 0) continue;

        if (SteamController::IsStateReportId(buf[0])) {
            if (calibrated)
                PrintStateDiff(buf, n);
            else if (stateCount++ % 25 == 0)
                PrintRawReport(buf, n);
        } else {
            PrintRawReport(buf, n);
        }
    }

    return 0;
}
