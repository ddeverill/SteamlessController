#include <Windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <climits>
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
// Calibration — collect idle frames to identify analog axis byte positions
// ---------------------------------------------------------------------------

static constexpr int   CALIB_FRAMES    = 200;
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

static void RunCalibration(SteamController& ctrl) {
    printf("--- Calibration: leave controller idle, collecting %d frames ---\n",
           CALIB_FRAMES);

    uint8_t buf[64];
    int collected = 0;
    while (collected < CALIB_FRAMES) {
        size_t n = ctrl.ReadReport(buf, sizeof(buf), 200);
        if (n == 0 || buf[0] != SteamController::REPORT_STATE)
            continue;
        if (s_reportLen == 0) s_reportLen = n;
        for (size_t i = 1; i < n; ++i)
            s_stats[i].feed(buf[i]);
        ++collected;
        if (collected % 50 == 0)
            printf("  %d / %d\n", collected, CALIB_FRAMES);
    }

    // Apply force-show overrides before marking axes.
    auto forceShow = [](size_t i) -> bool {
        if (FORCE_SHOW_BYTES_10_11 && (i == 10 || i == 11)) return true;
        if (FORCE_SHOW_BYTES_12_13 && (i == 12 || i == 13)) return true;
        if (FORCE_SHOW_BYTES_14_15 && (i == 14 || i == 15)) return true;
        if (FORCE_SHOW_BYTES_16_17 && (i == 16 || i == 17)) return true;
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
}

// ---------------------------------------------------------------------------
// Differential display — axes masked, only changes shown
// ---------------------------------------------------------------------------

static uint8_t s_prev42[64];
static bool    s_hasPrev42 = false;

static void PrintState42Diff(const uint8_t* buf, size_t n) {
    if (n < 2) return;

    bool anyDiff = false;
    for (size_t i = 1; i < n; ++i) {
        if (s_isAxis[i]) continue;
        bool changed = !s_hasPrev42 || (buf[i] != s_prev42[i]);
        if (changed) {
            if (!anyDiff) { printf("  [idx]  new  old\n"); anyDiff = true; }
            printf("  [%02zu]   %02X  (%02X)\n", i, buf[i], s_hasPrev42 ? s_prev42[i] : 0);
        }
    }

    memcpy(s_prev42, buf, n);
    s_hasPrev42 = true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void EnumerateAllValveDevices() {
    printf("=== All Valve HID interfaces (VID=28DE) ===\n");
    auto paths = HidDevice::Enumerate(SteamController::VALVE_VID, 0);
    if (paths.empty()) {
        printf("  None found.\n\n");
        return;
    }

    for (auto const& path : paths) {
        HANDLE h = CreateFileW(path.c_str(), 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs{};
        attrs.Size = sizeof(attrs);
        HidD_GetAttributes(h, &attrs);

        wchar_t productBuf[128] = L"(unknown)";
        HidD_GetProductString(h, productBuf, sizeof(productBuf));

        uint16_t usagePage = 0, usage = 0;
        PHIDP_PREPARSED_DATA preparsed;
        if (HidD_GetPreparsedData(h, &preparsed)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
                usagePage = caps.UsagePage;
                usage     = caps.Usage;
            }
            HidD_FreePreparsedData(preparsed);
        }

        printf("  PID=%04X  UsagePage=%04X  Usage=%04X  \"%ls\"\n",
               attrs.ProductID, usagePage, usage, productBuf);
        CloseHandle(h);
    }
    printf("\n");
}

int main() {
    signal(SIGINT,  OnSignal);
    signal(SIGTERM, OnSignal);

    printf("=== SteamProbe — 2026 Steam Controller (VID=28DE PID=1302) ===\n\n");
    printf("NOTE: Close Steam before running this tool.\n\n");

    EnumerateAllValveDevices();

    SteamController controller;
    g_controller = &controller;

    if (!controller.Open()) {
        fprintf(stderr, "Could not open device.\n");
        return 1;
    }

    printf("Disabling lizard mode...\n");
    if (!controller.DisableLizardMode()) {
        fprintf(stderr, "Lizard mode disable failed.\n");
        return 1;
    }
    printf("Lizard mode OFF.\n\n");

    RunCalibration(controller);

    uint8_t buf[64];
    while (true) {
        size_t n = controller.ReadReport(buf, sizeof(buf), 100);
        if (n == 0) continue;

        uint8_t reportId = buf[0];
        if (reportId == SteamController::REPORT_STATE) {
            PrintState42Diff(buf, n);
        } else {
            printf("[0x%02X %zu bytes] ", reportId, n);
            for (size_t i = 0; i < n; ++i) printf("%02X ", buf[i]);
            printf("\n");
        }
    }

    return 0;
}
