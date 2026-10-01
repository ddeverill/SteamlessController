#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include "BackButtonConfig.h"

// Simulated dual-stage triggers (#75).
//
// The 2026 controller's triggers are smooth analog travel with no physical
// second stage, so a "full press" is a software threshold on the analog value.
// TriggerProbe measured what that threshold has to live with:
//
//   - rest reads exactly 0, so there is no noise floor to clear;
//   - every deliberate pull peaks at exactly 100% and sits flat there, so the
//     top of travel is a firm place to put a threshold;
//   - the firmware's own full-pull bit (BTN_RT_FULL / BTN_LT_FULL) fires at
//     95-100% and lets go at about 80%, which is the click a hand would expect.
//
// The defaults below copy that, and the analog value is thresholded in software
// rather than reading the firmware bit so both triggers share one code path and
// the point stays adjustable.

// What a trigger does besides being analog. Values are stable — persisted as
// DWORDs, and Standard is 0 so a missing or zeroed value is exactly how every
// profile written before this existed behaved.
enum class TriggerMode : uint8_t {
    Standard  = 0,  // analog passthrough only
    DualStage = 1,  // analog passthrough, plus a binding held past the press point
    COUNT
};

inline TriggerMode TriggerModeFromDword(uint32_t v) {
    return v < static_cast<uint32_t>(TriggerMode::COUNT)
         ? static_cast<TriggerMode>(v)
         : TriggerMode::Standard;
}

// When the simulated click is felt. There is no actuator in the trigger, so the
// click comes from the controller body — a pad click and a grip buzz on the
// trigger's side, which hardware testing found the most click-like of the
// candidates (see SteamController::QueueTriggerClick).
enum class TriggerHaptic : uint8_t {
    Off             = 0,
    Press           = 1,
    PressAndRelease = 2,  // like a physical click: once down, once up
    COUNT
};

inline TriggerHaptic TriggerHapticFromDword(uint32_t v) {
    return v < static_cast<uint32_t>(TriggerHaptic::COUNT)
         ? static_cast<TriggerHaptic>(v)
         : TriggerHaptic::PressAndRelease;
}

// Wire ids for the settings page, which speaks in strings. Anything unknown —
// including an empty or missing value — lands on the safe default, so a page
// that predates a setting cannot turn dual-stage on by omission.
inline const char* TriggerModeId(TriggerMode m) {
    return m == TriggerMode::DualStage ? "dual" : "standard";
}
inline TriggerMode TriggerModeFromId(const std::string& id) {
    return id == "dual" ? TriggerMode::DualStage : TriggerMode::Standard;
}

inline const char* TriggerHapticId(TriggerHaptic h) {
    switch (h) {
    case TriggerHaptic::Off:   return "off";
    case TriggerHaptic::Press: return "press";
    default:                   return "both";
    }
}
inline TriggerHaptic TriggerHapticFromId(const std::string& id) {
    if (id == "off")   return TriggerHaptic::Off;
    if (id == "press") return TriggerHaptic::Press;
    return TriggerHaptic::PressAndRelease;
}

// Raw trigger range in the state report: 0x0000 released to 0x7FFF fully pulled.
inline constexpr int kTriggerRawMax = 0x7FFF;

// Percent of travel. A threshold below ~20% would fire on a trigger resting
// under a finger, and the release has to leave the press room above it.
inline constexpr uint32_t kTriggerPressDefault   = 95;
inline constexpr uint32_t kTriggerReleaseDefault = 80;
inline constexpr uint32_t kTriggerPressMin       = 20;
inline constexpr uint32_t kTriggerPressMax       = 100;
inline constexpr uint32_t kTriggerReleaseMin     = 5;
// Closest the release may sit to the press. With none, a trigger resting right
// on the press point would chatter on every report.
inline constexpr uint32_t kTriggerHysteresisMin  = 3;

inline uint32_t ClampTriggerPress(uint32_t v) {
    return std::clamp(v, kTriggerPressMin, kTriggerPressMax);
}

// Always strictly below the (already clamped) press point.
inline uint32_t ClampTriggerRelease(uint32_t release, uint32_t press) {
    const uint32_t ceiling = press > kTriggerHysteresisMin ? press - kTriggerHysteresisMin
                                                           : kTriggerReleaseMin;
    // (std::max) in parentheses: this header is included from files that pull in
    // Windows.h without NOMINMAX, whose max macro would eat the call.
    return std::clamp(release, kTriggerReleaseMin, (std::max)(ceiling, kTriggerReleaseMin));
}

// A percentage that arrived as text, where zero or nothing parseable means "not
// sent" rather than "zero percent". The same convention as the scroll speed: a
// missing value has to land on the default, not be clamped up to the minimum.
inline uint32_t TriggerPressFromWire(uint32_t v) {
    return v == 0 ? kTriggerPressDefault : ClampTriggerPress(v);
}
inline uint32_t TriggerReleaseFromWire(uint32_t v, uint32_t press) {
    return v == 0 ? ClampTriggerRelease(kTriggerReleaseDefault, press)
                  : ClampTriggerRelease(v, press);
}

// One trigger's configuration. The full press is a BackButtonBinding rather
// than a type of its own for the same reason a trackpad click is: it is the
// same question ("what should this input do"), answered by the same catalog and
// dispatched down the same path as the paddles.
struct TriggerSettings {
    TriggerMode       mode    = TriggerMode::Standard;
    uint32_t          press   = kTriggerPressDefault;    // percent of travel
    uint32_t          release = kTriggerReleaseDefault;  // percent of travel
    TriggerHaptic     haptic  = TriggerHaptic::PressAndRelease;
    BackButtonBinding full    = BackButtonBinding::FromAction(BackButtonAction::None);

    bool operator==(const TriggerSettings& o) const {
        return mode == o.mode && press == o.press && release == o.release
            && haptic == o.haptic && full == o.full;
    }
    bool operator!=(const TriggerSettings& o) const { return !(*this == o); }

    bool IsDualStage() const { return mode == TriggerMode::DualStage; }

    // The binding to hold while the trigger is past its press point, or nothing
    // at all outside dual-stage mode. Reading it through here rather than the
    // field is what stops a binding left over from an earlier mode firing from
    // behind hidden UI.
    BackButtonBinding EffectiveFull() const {
        return IsDualStage() ? full : BackButtonBinding{};
    }

    bool ClicksOnPress() const {
        return IsDualStage() && haptic != TriggerHaptic::Off;
    }
    bool ClicksOnRelease() const {
        return IsDualStage() && haptic == TriggerHaptic::PressAndRelease;
    }
};

// Whether one trigger is past its press point, with hysteresis: it presses at
// `press` and does not let go until it has come back down to `release`.
//
// Hysteresis is not optional. The firmware's own bit has about 20 points of it
// for exactly this reason — a finger resting near a single threshold would
// otherwise press and release the binding on every report.
class TriggerFullPress {
public:
    // raw is 0..kTriggerRawMax. Returns whether the full press is held.
    bool Update(int raw, const TriggerSettings& s) {
        if (!s.IsDualStage()) {
            // Leaving dual-stage mode while held reports a release this frame,
            // which is what lets the dispatcher let go of what the press sent.
            held_ = false;
            return false;
        }
        const uint32_t press   = ClampTriggerPress(s.press);
        const uint32_t release = ClampTriggerRelease(s.release, press);
        const int pressAt   = RawAt(press);
        const int releaseAt = RawAt(release);
        if (!held_ && raw >= pressAt)        held_ = true;
        else if (held_ && raw <= releaseAt)  held_ = false;
        return held_;
    }

    void Reset() { held_ = false; }
    bool Held() const { return held_; }

    // Percent of travel as a raw value, rounded up so a threshold never fires
    // a hair early.
    static int RawAt(uint32_t percent) {
        return static_cast<int>((static_cast<uint64_t>(percent) * kTriggerRawMax + 99) / 100);
    }

private:
    bool held_ = false;
};

// ---------------------------------------------------------------------------
// Persistence
//
// The registry is read from two places that cannot share a helper — the default
// profile in TrayApp and every game profile in GameProfiles — and each already
// carries its own copy of the per-pad names. Written once here, so a trigger's
// values cannot drift apart between the two.
//
// readDw(name, default) -> value; writeDw(name, value). Names are the prefix
// plus Mode / Press / Release / Haptic / Full, e.g. "LeftTriggerMode".
//
// Every value is absent from a profile written before this existed, and the
// defaults make that read back as Standard — a trigger that is just analog,
// which is exactly what those profiles did.
// ---------------------------------------------------------------------------

template <class ReadDw>
inline TriggerSettings ReadTriggerSettings(const wchar_t* prefix, ReadDw&& readDw) {
    auto name = [&](const wchar_t* suffix) { return std::wstring(prefix) + suffix; };
    TriggerSettings t;
    t.mode    = TriggerModeFromDword(readDw(name(L"Mode").c_str(), 0u));
    t.press   = ClampTriggerPress(readDw(name(L"Press").c_str(), kTriggerPressDefault));
    t.release = ClampTriggerRelease(readDw(name(L"Release").c_str(), kTriggerReleaseDefault),
                                    t.press);
    t.haptic  = TriggerHapticFromDword(readDw(name(L"Haptic").c_str(),
                    static_cast<uint32_t>(TriggerHaptic::PressAndRelease)));
    t.full    = BackButtonBinding::Unpack(readDw(name(L"Full").c_str(),
                    BackButtonBinding::FromAction(BackButtonAction::None).Pack()));
    return t;
}

template <class WriteDw>
inline void WriteTriggerSettings(const wchar_t* prefix, const TriggerSettings& t,
                                 WriteDw&& writeDw) {
    auto name = [&](const wchar_t* suffix) { return std::wstring(prefix) + suffix; };
    writeDw(name(L"Mode").c_str(),    static_cast<uint32_t>(t.mode));
    writeDw(name(L"Press").c_str(),   t.press);
    writeDw(name(L"Release").c_str(), t.release);
    writeDw(name(L"Haptic").c_str(),  static_cast<uint32_t>(t.haptic));
    writeDw(name(L"Full").c_str(),    t.full.Pack());
}
