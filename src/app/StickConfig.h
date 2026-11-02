#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

// Stick deadzone (#117).
//
// The 2026 controller's sticks are reported exactly as the hardware reads them,
// and a hand-off stick does not read exactly zero. Anything that takes the pad's
// word for it — an emulator waiting for "press the input to bind", a gamepad
// tester — sees the sticks moving all the time, and a bind prompt latches onto
// them before a button can be pressed. XInput games mostly hide this behind a
// deadzone of their own, which is why it only shows up elsewhere.
//
// So the virtual pad applies one blanket deadzone, here, to both platforms. It
// is fixed rather than a setting: a stick does not need tuning per game the way
// a trigger threshold does, and StickRestProbe measures what it has to clear.
//
// What it measured, hands off, on two units: the resting reading is a steady
// offset from zero rather than noise — jitter about the mean is under 0.5% —
// and the offset is as large as 5.9% of the range on one stick, wandering by
// over a point from one settle to the next. A deadzone has to clear the offset,
// not the jitter, and has to leave room for the stick to come to rest somewhere
// a little worse than it was measured.

// Raw stick range in the state report: -0x8000 .. 0x7FFF, centre at zero.
inline constexpr int kStickRawMax = 0x7FFF;

// Percent of full deflection that reads as no input. About 1.4 times the worst
// resting reading measured (5.87%); wider costs fine control for no benefit.
inline constexpr int kStickDeadzonePercent = 8;
inline constexpr int kStickDeadzoneRaw     = kStickRawMax * kStickDeadzonePercent / 100;

struct StickPos {
    int16_t x = 0;
    int16_t y = 0;
};

// Radial deadzone with the remaining travel rescaled to the full range.
//
// Radial rather than per-axis: a per-axis deadzone turns a stick held nearly
// straight up into a perfectly straight one, and snaps diagonals toward the
// axes. A circle leaves the direction alone and only decides whether there is
// any input.
//
// Rescaled rather than just cut: otherwise the output jumps from nothing to the
// size of the deadzone the instant the stick leaves it, and the top of the range
// is lost. The scale is applied to each axis rather than to the vector's length,
// and each axis is clamped on its own, so a diagonal pushed into a stick's
// corner — whose length is past a circle's edge — still reaches full on both
// axes as it did before.
//
// `outer` is where the ramp ends: a stick pushed that far reads as full, and
// anything past it too (#131). At the top of the range, the default, the ramp
// is the one it has always been.
inline StickPos ApplyStickDeadzone(int16_t x, int16_t y,
                                   int deadzone = kStickDeadzoneRaw,
                                   int outer = kStickRawMax) {
    // Parenthesised throughout: this header is reached through files that
    // include Windows.h first, whose min and max macros would eat the calls.
    //
    // Nothing to do: no deadzone, and the ramp already ends at the top.
    if (deadzone <= 0 && outer >= kStickRawMax) return { x, y };
    // A deadzone as wide as the whole range would leave nothing to rescale into,
    // and a ramp has to end somewhere past where it starts.
    const int dzRaw  = (std::clamp)(deadzone, 0, kStickRawMax - 1);
    const double dz  = static_cast<double>(dzRaw);
    const double top = static_cast<double>((std::clamp)(outer, dzRaw + 1, kStickRawMax));

    const double mag = std::hypot(static_cast<double>(x), static_cast<double>(y));
    if (mag <= dz) return {};

    const double full  = static_cast<double>(kStickRawMax);
    const double scale = (mag - dz) / (top - dz) * full / mag;

    auto axis = [&](int16_t v) {
        const long scaled = std::lround(static_cast<double>(v) * scale);
        return static_cast<int16_t>(std::clamp<long>(scaled, -32768L, 32767L));
    };
    return { axis(x), axis(y) };
}

// The same deadzone and ramp, but each axis on its own (#131). A square
// deadzone: the stick can be pushed all the way over on one axis and still read
// nothing on the other, and a diagonal reaches full on both axes at once rather
// than 71% of it on each. What a circle deliberately does not do — see above —
// is exactly what this is for, which is why it is offered only to people who
// ask for it.
inline StickPos ApplyStickDeadzoneSquare(int16_t x, int16_t y,
                                         int deadzone = kStickDeadzoneRaw,
                                         int outer = kStickRawMax) {
    const int dzRaw  = (std::clamp)(deadzone, 0, kStickRawMax - 1);
    const double dz  = static_cast<double>(dzRaw);
    const double top = static_cast<double>((std::clamp)(outer, dzRaw + 1, kStickRawMax));
    const double full = static_cast<double>(kStickRawMax);

    auto axis = [&](int16_t v) -> int16_t {
        const double m = std::fabs(static_cast<double>(v));
        if (m <= dz) return 0;
        const double t = (m - dz) / (top - dz);
        // The whole of the bottom of the range is at or past full, so it is
        // full — including the one value, -32768, that +32767 cannot negate.
        if (t >= 1.0) return v < 0 ? static_cast<int16_t>(-32768)
                                   : static_cast<int16_t>(32767);
        const long scaled = std::lround(t * full);
        return static_cast<int16_t>(v < 0 ? -scaled : scaled);
    };
    return { axis(x), axis(y) };
}

// How one stick is read (#131). Default is the 8% circle every install has
// always had and takes no settings at all; Circle and Square are for people who
// want to tune it, and carry the two points of Steam's own deadzone control —
// where the deadzone ends and the ramp begins (`inner`), and where the ramp ends
// and full output begins (`outer`), both as a percent of the stick's travel.
//
// Circle at 8 and 100 is the Default exactly, so switching to it changes
// nothing until a point is moved.
enum class StickDeadzoneMode : uint8_t {
    Default = 0,
    Circle  = 1,
    Square  = 2,
};

inline constexpr uint32_t kStickInnerDefault = 8;
inline constexpr uint32_t kStickOuterDefault = 100;
// The two points never get closer than this: the ramp between them is what
// turns a push into output, and one squeezed to nothing is a switch.
inline constexpr uint32_t kStickMinGap = 5;

inline StickDeadzoneMode StickDeadzoneModeFromDword(uint32_t v) {
    return v == static_cast<uint32_t>(StickDeadzoneMode::Circle) ? StickDeadzoneMode::Circle
         : v == static_cast<uint32_t>(StickDeadzoneMode::Square) ? StickDeadzoneMode::Square
                                                                 : StickDeadzoneMode::Default;
}

inline const char* StickDeadzoneModeId(StickDeadzoneMode m) {
    switch (m) {
    case StickDeadzoneMode::Circle: return "circle";
    case StickDeadzoneMode::Square: return "square";
    default:                        return "default";
    }
}

inline StickDeadzoneMode StickDeadzoneModeFromId(const std::string& id) {
    if (id == "circle") return StickDeadzoneMode::Circle;
    if (id == "square") return StickDeadzoneMode::Square;
    return StickDeadzoneMode::Default;
}

// Both points together, because each one's limit depends on the other. The
// inner point is the one held in place when they collide: it is the one the
// user is more likely to have chosen deliberately.
inline void ClampStickRange(uint32_t& inner, uint32_t& outer) {
    inner = (std::min)(inner, 100u - kStickMinGap);
    outer = (std::clamp)(outer, inner + kStickMinGap, 100u);
}

struct StickSettings {
    StickDeadzoneMode mode  = StickDeadzoneMode::Default;
    uint32_t          inner = kStickInnerDefault;
    uint32_t          outer = kStickOuterDefault;

    bool operator==(const StickSettings& o) const {
        return mode == o.mode && inner == o.inner && outer == o.outer;
    }
    bool operator!=(const StickSettings& o) const { return !(*this == o); }
};

// One stick's X and Y through whatever its settings say.
inline StickPos ApplyStick(int16_t x, int16_t y, const StickSettings& s) {
    if (s.mode == StickDeadzoneMode::Default) return ApplyStickDeadzone(x, y);

    uint32_t inner = s.inner, outer = s.outer;
    ClampStickRange(inner, outer);
    const int innerRaw = static_cast<int>(kStickRawMax * inner / 100);
    const int outerRaw = static_cast<int>(kStickRawMax * outer / 100);
    return s.mode == StickDeadzoneMode::Square
         ? ApplyStickDeadzoneSquare(x, y, innerRaw, outerRaw)
         : ApplyStickDeadzone(x, y, innerRaw, outerRaw);
}

// Registry and profile storage, shared by the default profile and the per-game
// ones so the two cannot drift apart — the same arrangement the triggers use.
// Names are the prefix plus Mode / Inner / Outer, e.g. "LeftStickMode".
//
// Absent from every profile written before this existed, and the defaults make
// that read back as Default: the 8% circle, as those profiles always had.
template <class ReadDw>
inline StickSettings ReadStickSettings(const wchar_t* prefix, ReadDw&& readDw) {
    auto name = [&](const wchar_t* suffix) { return std::wstring(prefix) + suffix; };
    StickSettings s;
    s.mode  = StickDeadzoneModeFromDword(readDw(name(L"Mode").c_str(), 0u));
    s.inner = readDw(name(L"Inner").c_str(), kStickInnerDefault);
    s.outer = readDw(name(L"Outer").c_str(), kStickOuterDefault);
    ClampStickRange(s.inner, s.outer);
    return s;
}

template <class WriteDw>
inline void WriteStickSettings(const wchar_t* prefix, const StickSettings& s,
                               WriteDw&& writeDw) {
    auto name = [&](const wchar_t* suffix) { return std::wstring(prefix) + suffix; };
    writeDw(name(L"Mode").c_str(),  static_cast<uint32_t>(s.mode));
    writeDw(name(L"Inner").c_str(), s.inner);
    writeDw(name(L"Outer").c_str(), s.outer);
}
