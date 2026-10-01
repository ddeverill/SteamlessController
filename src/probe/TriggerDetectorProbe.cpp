// Console diagnostic for dual-stage triggers (#75): the full-press detector's
// hysteresis, what it does when the mode is switched mid-hold, the clamps on the
// persisted thresholds, and that profiles written before this existed read back
// as a plain analog trigger. None of it needs a controller.
//
// The press and release points, and why they are 95/80, come from TriggerProbe's
// measurements of real hardware; this checks that the detector honours whatever
// numbers it is given, which a typo in a comparison would get quietly wrong.
#include "app/TriggerConfig.h"
#include <cstdio>
#include <map>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) ++g_failures;
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
}

TriggerSettings Dual(uint32_t press = kTriggerPressDefault,
                     uint32_t release = kTriggerReleaseDefault) {
    TriggerSettings t;
    t.mode    = TriggerMode::DualStage;
    t.press   = press;
    t.release = release;
    return t;
}

int Raw(double percent) {
    return static_cast<int>(percent / 100.0 * kTriggerRawMax);
}

// A fake registry: the codec only sees a name->value callable.
struct FakeRegistry {
    std::map<std::wstring, uint32_t> values;
    uint32_t Read(const wchar_t* name, uint32_t def) const {
        const auto it = values.find(name);
        return it == values.end() ? def : it->second;
    }
    void Write(const wchar_t* name, uint32_t v) { values[name] = v; }
};

}  // namespace

int main() {
    printf("Press and release\n");
    {
        TriggerFullPress d;
        const TriggerSettings t = Dual();
        Check(!d.Update(Raw(0),  t), "released trigger is not pressed");
        Check(!d.Update(Raw(50), t), "half pull is not pressed");
        Check(!d.Update(Raw(94), t), "just under the press point is not pressed");
        Check( d.Update(Raw(96), t), "past the press point presses");
        Check( d.Update(Raw(100), t), "pinned at full stays pressed");
        Check( d.Update(Raw(85), t), "easing off to 85% stays pressed (hysteresis)");
        Check( d.Update(Raw(81), t), "81% is still above the release point");
        Check(!d.Update(Raw(79), t), "below the release point releases");
        Check(!d.Update(Raw(90), t), "back up to 90% does not re-press — that is under 95");
        Check( d.Update(Raw(96), t), "and 96% does");
    }

    printf("\nExact edges, because a threshold is rounded\n");
    {
        TriggerFullPress d;
        const TriggerSettings t = Dual(100, 80);
        Check(!d.Update(kTriggerRawMax - 1, t), "one short of full does not press a 100% point");
        Check( d.Update(kTriggerRawMax, t),     "full travel presses a 100% point");
        // The rounding is up, so a threshold never fires a hair early.
        Check(TriggerFullPress::RawAt(95) >= static_cast<int>(0.95 * kTriggerRawMax),
              "95% rounds up, never early");
        Check(TriggerFullPress::RawAt(100) == kTriggerRawMax, "100% is exactly full travel");
    }

    printf("\nChatter\n");
    {
        TriggerFullPress d;
        const TriggerSettings t = Dual();
        d.Update(Raw(100), t);
        int transitions = 0;
        bool last = true;
        // A finger wobbling +/-5 points around 90% — between the two points,
        // so a single threshold would flip on every report and this must not.
        for (int i = 0; i < 200; ++i) {
            const bool now = d.Update(Raw(90 + ((i % 2) ? 5 : -5)), t);
            if (now != last) ++transitions;
            last = now;
        }
        Check(transitions == 0, "wobble between the points never releases a held press");
    }

    printf("\nMode changes\n");
    {
        TriggerFullPress d;
        TriggerSettings t = Dual();
        d.Update(Raw(100), t);
        t.mode = TriggerMode::Standard;
        Check(!d.Update(Raw(100), t), "switching to Standard while held reports a release");
        Check(!d.Update(Raw(100), t), "and stays released with the trigger still down");
        t.mode = TriggerMode::DualStage;
        Check( d.Update(Raw(100), t), "switching back with the trigger down presses again");
        Check(!TriggerSettings{}.IsDualStage(), "a default trigger is Standard");
        Check(TriggerSettings{}.EffectiveFull() == BackButtonBinding{},
              "Standard exposes no binding even if one is stored");
        TriggerSettings stored;
        stored.full = BackButtonBinding::FromKey('K');
        Check(stored.EffectiveFull() == BackButtonBinding{},
              "a binding left over from dual-stage does not fire in Standard");
        stored.mode = TriggerMode::DualStage;
        Check(stored.EffectiveFull() == BackButtonBinding::FromKey('K'),
              "and fires again in DualStage");
    }

    printf("\nHostile thresholds are made safe\n");
    {
        TriggerFullPress d;
        // Release above press would never let go; the clamp has to repair it.
        const TriggerSettings inverted = Dual(60, 90);
        d.Update(Raw(100), inverted);
        Check(!d.Update(Raw(10), inverted), "an inverted release still releases at the bottom");
        Check(ClampTriggerRelease(90, 60) < 60, "release is forced below press");
        Check(ClampTriggerRelease(59, 60) <= 60 - kTriggerHysteresisMin,
              "and leaves the minimum hysteresis");
        Check(ClampTriggerPress(0)   == kTriggerPressMin, "a zero press point is raised");
        Check(ClampTriggerPress(500) == kTriggerPressMax, "an absurd press point is capped");
        Check(ClampTriggerRelease(0, 95) >= kTriggerReleaseMin, "a zero release is raised");

        TriggerFullPress z;
        Check(!z.Update(0, Dual(1, 0)), "a trigger at rest never presses, whatever the settings");
    }

    printf("\nPersistence\n");
    {
        TriggerSettings t = Dual(97, 70);
        t.haptic = TriggerHaptic::Press;
        t.full   = BackButtonBinding::FromKey('K', BackButtonBinding::ModCtrl);

        FakeRegistry reg;
        WriteTriggerSettings(L"RightTrigger", t,
            [&](const wchar_t* n, uint32_t v) { reg.Write(n, v); });
        const TriggerSettings back = ReadTriggerSettings(L"RightTrigger",
            [&](const wchar_t* n, uint32_t d) { return reg.Read(n, d); });
        Check(back == t, "settings survive a write and read");

        const TriggerSettings otherSide = ReadTriggerSettings(L"LeftTrigger",
            [&](const wchar_t* n, uint32_t d) { return reg.Read(n, d); });
        Check(otherSide == TriggerSettings{}, "the other trigger is untouched by that write");
        Check(reg.values.count(L"RightTriggerMode") == 1
              && reg.values.count(L"RightTriggerFull") == 1,
              "values are stored under the trigger's prefix");
    }

    printf("\nProfiles written before dual-stage triggers existed\n");
    {
        FakeRegistry empty;  // exactly what an old profile has: no trigger values
        const TriggerSettings old = ReadTriggerSettings(L"LeftTrigger",
            [&](const wchar_t* n, uint32_t d) { return empty.Read(n, d); });
        Check(old.mode == TriggerMode::Standard, "absent values read back as Standard");
        Check(old == TriggerSettings{}, "and as exactly the defaults");
        Check(old.press == 95 && old.release == 80, "defaults are the measured 95/80");

        FakeRegistry future;
        future.values[L"LeftTriggerMode"]   = 99;  // a mode a newer build invented
        future.values[L"LeftTriggerHaptic"] = 99;
        const TriggerSettings f = ReadTriggerSettings(L"LeftTrigger",
            [&](const wchar_t* n, uint32_t d) { return future.Read(n, d); });
        Check(f.mode == TriggerMode::Standard, "a mode from a newer build degrades to Standard");
        Check(f.haptic == TriggerHaptic::PressAndRelease,
              "a haptic setting from a newer build degrades to the default");

        FakeRegistry corrupt;
        corrupt.values[L"LeftTriggerPress"]   = 0;
        corrupt.values[L"LeftTriggerRelease"] = 100000;
        const TriggerSettings c = ReadTriggerSettings(L"LeftTrigger",
            [&](const wchar_t* n, uint32_t d) { return corrupt.Read(n, d); });
        Check(c.press >= kTriggerPressMin && c.release < c.press,
              "nonsense thresholds read back clamped, with release below press");
    }

    printf("\nWire ids for the settings page\n");
    {
        Check(TriggerModeFromId(TriggerModeId(TriggerMode::DualStage)) == TriggerMode::DualStage,
              "dual-stage mode round-trips through its id");
        Check(TriggerModeFromId("") == TriggerMode::Standard, "an empty mode id is Standard");
        Check(TriggerModeFromId("garbage") == TriggerMode::Standard,
              "an unknown mode id is Standard, never dual by accident");
        for (TriggerHaptic h : { TriggerHaptic::Off, TriggerHaptic::Press,
                                 TriggerHaptic::PressAndRelease })
            Check(TriggerHapticFromId(TriggerHapticId(h)) == h, "each haptic id round-trips");
        Check(TriggerHapticFromId("") == TriggerHaptic::PressAndRelease,
              "an empty haptic id is the default");

        // A value the page did not send arrives as 0 and has to mean "default",
        // not "clamp up to the minimum".
        Check(TriggerPressFromWire(0) == kTriggerPressDefault, "unsent press is the default");
        Check(TriggerReleaseFromWire(0, 95) == kTriggerReleaseDefault, "unsent release is the default");
        Check(TriggerReleaseFromWire(0, 60) < 60, "an unsent release still ends up below a lower press");
        Check(TriggerPressFromWire(92) == 92, "a sent press is kept");
        Check(TriggerPressFromWire(3) == kTriggerPressMin, "a too-small press is raised");
        Check(TriggerReleaseFromWire(99, 92) == 92 - kTriggerHysteresisMin,
              "a release above the press is forced below it");
    }

    printf("\nClick haptic gating\n");
    {
        TriggerSettings t = Dual();
        t.haptic = TriggerHaptic::PressAndRelease;
        Check(t.ClicksOnPress() && t.ClicksOnRelease(), "press-and-release clicks both ways");
        t.haptic = TriggerHaptic::Press;
        Check(t.ClicksOnPress() && !t.ClicksOnRelease(), "press-only does not click on release");
        t.haptic = TriggerHaptic::Off;
        Check(!t.ClicksOnPress() && !t.ClicksOnRelease(), "off never clicks");
        t.haptic = TriggerHaptic::PressAndRelease;
        t.mode = TriggerMode::Standard;
        Check(!t.ClicksOnPress() && !t.ClicksOnRelease(),
              "a Standard trigger never clicks, whatever its haptic setting");
    }

    printf("\n%s (%d failure(s))\n", g_failures ? "FAILED" : "All checks passed", g_failures);
    return g_failures ? 1 : 0;
}
