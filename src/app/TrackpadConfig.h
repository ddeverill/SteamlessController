#pragma once
#include <cstdint>
#include <string>
#include "BackButtonConfig.h"
#include "ControllerPlatform.h"

// What a trackpad's finger movement drives.
//
// Values are stable — persisted as DWORDs. None is 0 so a missing or zeroed
// value reads as "this pad does nothing", which is both the safe default and
// what an unconfigured pad did on Xbox, the default platform.
enum class TrackpadMode : uint8_t {
    None         = 0,  // pad is not used for anything
    MousePointer = 1,
    ScrollWheel  = 2,
    // Pad feeds the virtual DS4's touchpad, which only exists in PlayStation
    // mode — the X360 report has nothing to carry it. Its click is the
    // touchpad press by definition and is therefore not rebindable; letting
    // it be both would report one physical press two different ways.
    DS4Touchpad  = 3,
    // Pad is four directional buttons. Which one a press means is decided by
    // where on the pad the thumb is — see PadZone below.
    DirectionalPad = 4,
    // Pad is one big button: no directions and no movement, just the click
    // and touch bindings every mode but None and DS4Touchpad offers.
    SingleButton   = 5,
    COUNT
};

inline const char* TrackpadModeId(TrackpadMode m) {
    switch (m) {
    case TrackpadMode::MousePointer:   return "pointer";
    case TrackpadMode::ScrollWheel:    return "scroll";
    case TrackpadMode::DS4Touchpad:    return "ds4";
    case TrackpadMode::DirectionalPad: return "dpad";
    case TrackpadMode::SingleButton:   return "button";
    default:                           return "none";
    }
}

inline TrackpadMode TrackpadModeFromId(const std::string& id) {
    if (id == "pointer") return TrackpadMode::MousePointer;
    if (id == "scroll")  return TrackpadMode::ScrollWheel;
    if (id == "ds4")     return TrackpadMode::DS4Touchpad;
    if (id == "dpad")    return TrackpadMode::DirectionalPad;
    if (id == "button")  return TrackpadMode::SingleButton;
    return TrackpadMode::None;
}

// Anything a future build might write that this one does not understand
// degrades to "does nothing" rather than being applied.
inline TrackpadMode TrackpadModeFromDword(uint32_t v) {
    return v < static_cast<uint32_t>(TrackpadMode::COUNT)
         ? static_cast<TrackpadMode>(v)
         : TrackpadMode::None;
}

// Which way the content moves when a finger drags across a pad in scroll
// mode. Natural is the touchscreen convention — the content follows the
// finger — and is the default.
enum class ScrollDirection : uint8_t { Natural = 0, Reversed = 1, COUNT };

inline const char* ScrollDirectionId(ScrollDirection d) {
    return d == ScrollDirection::Reversed ? "reversed" : "natural";
}

inline ScrollDirection ScrollDirectionFromId(const std::string& id) {
    return id == "reversed" ? ScrollDirection::Reversed : ScrollDirection::Natural;
}

inline ScrollDirection ScrollDirectionFromDword(uint32_t v) {
    return v < static_cast<uint32_t>(ScrollDirection::COUNT)
         ? static_cast<ScrollDirection>(v)
         : ScrollDirection::Natural;
}

// How fast a pad in scroll mode scrolls, as a percentage of the built-in
// scale. A taste setting: kCalibratedWheelLines in InputInjection.h already
// makes a given swipe scroll the same distance on every machine, and this is
// what remains for people who want that distance to be a different one.
//
// Stored as a DWORD, so 0 is what every profile written before this existed
// reads back as — which has to mean "unset", not "do not scroll at all".
// ScrollSpeedFromDword maps it to the default rather than clamping it up.
inline constexpr uint32_t kScrollSpeedDefault = 100;
inline constexpr uint32_t kScrollSpeedMin     = 10;
inline constexpr uint32_t kScrollSpeedMax     = 400;

inline constexpr uint32_t ClampScrollSpeed(uint32_t v) {
    if (v < kScrollSpeedMin) return kScrollSpeedMin;
    if (v > kScrollSpeedMax) return kScrollSpeedMax;
    return v;
}

inline uint32_t ScrollSpeedFromDword(uint32_t v) {
    return v == 0 ? kScrollSpeedDefault : ClampScrollSpeed(v);
}

// Whether a directional pad's diagonals press two directions at once (what a
// real d-pad does, and what makes diagonal movement in a game possible) or
// resolve to the nearer single direction (steadier for menus). Eight-way is
// the default.
enum class DiagonalMode : uint8_t { EightWay = 0, FourWay = 1, COUNT };

inline const char* DiagonalModeId(DiagonalMode d) {
    return d == DiagonalMode::FourWay ? "four" : "eight";
}

inline DiagonalMode DiagonalModeFromId(const std::string& id) {
    return id == "four" ? DiagonalMode::FourWay : DiagonalMode::EightWay;
}

inline DiagonalMode DiagonalModeFromDword(uint32_t v) {
    return v < static_cast<uint32_t>(DiagonalMode::COUNT)
         ? static_cast<DiagonalMode>(v)
         : DiagonalMode::EightWay;
}

// The four directions a pad in DirectionalPad mode can press, as a bitmask so
// a diagonal is one value rather than two booleans that have to travel
// together.
enum PadDir : uint8_t {
    DirNone  = 0,
    DirUp    = 1 << 0,
    DirDown  = 1 << 1,
    DirLeft  = 1 << 2,
    DirRight = 1 << 3,
};

// What both pads resolved to this frame, for the two places a pad binding is
// delivered — the virtual controller and SendInput. Neither can work these out
// for itself: a direction and a click's zone both need the hysteresis and
// latch state that lives per-pad in TrackpadInput.
//
// Carrying the zone here rather than re-deriving it is what keeps the two
// delivery paths agreeing. It is inferrable — a directional pad only presses
// directions from a ring click, so a click with nothing under it is a centre
// one — but an inference that holds "except for one frame after the diagonals
// setting changes" is exactly the kind that gets found the hard way.
struct PadDigital {
    uint8_t leftDirs  = DirNone;
    uint8_t rightDirs = DirNone;
    // True for any pad that is not a directional pad, so a caller can gate the
    // click binding on this without asking what mode the pad is in.
    bool leftClickInCentre  = true;
    bool rightClickInCentre = true;
    // A tap just happened: a contact that ended without ever clicking. Held
    // true for a few frames so it dispatches as an ordinary press and release
    // — the event is over by the time it is known to have happened. Not the
    // report's touch bit, which a press also sets on its way down; see
    // Slot::TapState in ControllerManager for why that bit cannot be bound.
    bool leftTap  = false;
    bool rightTap = false;
    // The pad is being pressed, worked out from contact area rather than taken
    // from the firmware's click bit, which reports fewer than half of them.
    // See kPadPressArea.
    bool leftPressed  = false;
    bool rightPressed = false;
};

// Where a click landed on a pad, which is what keeps a directional pad's three
// signals disjoint: a click in the middle is the Trackpad Click binding, a
// click out in the ring is the direction under the thumb, and touch is not
// zoned at all. Without this split one physical press would have to mean both
// a direction and a click.
//
// The boundary is in raw report units rather than a fraction of the pad,
// because the pad's coordinate space is square — both axes saturate at 32767,
// so the distance to a corner (46341) is 1.41x the distance to an edge and no
// single fraction describes the same circle.
//
// 12000 comes from TrackpadZoneProbe (src/probe/TrackpadZoneProbe.cpp) run
// against real presses: deliberate centre presses reached at most 6035 and the
// loosest direction press was 17731, eyes-free. That leaves roughly a 2x
// margin on the centre side and 1.5x on the ring side. Roughly a 15mm circle
// on the physical pad.
inline constexpr int kPadRingRadius = 12000;

// Contact area above which a thumb is pressing rather than resting.
//
// The firmware's own click bit cannot be trusted for this: measured with
// TrackpadZoneProbe, a thumb that stays on the pad gets about nine of every
// twenty presses reported as clicks. The presses are all in the report —
// pressing flattens the fingertip and the contact area shows every one — so
// they are recovered from the area instead.
//
// One threshold, not two. Above it is pressed, below it is not, and everything
// below counts as backed off however heavily a particular thumb rests. A
// separate "released" level would be a second per-person number, and one
// guessed too high latches the detector and swallows every press after the
// first — exactly the bug the click haptic had.
//
// 1800, from TrackpadDirectionProbe --live traced against the firmware's click
// bit. Four bands, none of which overlap:
//
//   thumb resting or gliding      259..381    must not read as a press
//   presses the firmware misses  2840..3921
//   presses it calls a click     4026..4427   (it releases again around 2200)
//   a press being held           4671..14784  the area climbs while held
//
// So the empty span between a thumb that is merely down and one that is
// pressing runs from roughly 400 to 2800, and this sits in the middle of it:
// about five times a resting thumb, and still low enough to recover the
// presses the click bit drops, which is the whole reason the area is read.
//
// The previous 250 was calibrated against a thumb held OFF the pad — median 0,
// max 33 — rather than one resting on it, which put it inside the resting
// band. A thumb doing nothing read as a held press, and since the area never
// fell back through it, consecutive clicks merged into a single press: the
// swallowing described above, reached from a threshold too low rather than one
// too high. Traces do not support the dip that reasoning also rested on —
// while a press is held the area only climbs, and the apparent dips were
// separate press attempts merged into one span by that same low threshold.
inline constexpr int kPadPressArea = 1800;

// Frames the area must hold one side of the threshold before the answer
// changes. Not a per-person number — it describes the sensor's noise rather
// than anyone's grip — and it is what a single threshold needs in place of the
// second level it does without. Two frames is 8ms; three starts merging
// genuinely separate presses that arrive in quick succession.
inline constexpr int kPadPressFrames = 2;

// One physical trackpad's configuration. The click is a BackButtonBinding
// rather than a type of its own: a pad click and a back paddle are the same
// question ("what should this button do"), answered by the same catalog and
// dispatched down the same path.
struct TrackpadSettings {
    TrackpadMode      mode      = TrackpadMode::None;
    ScrollDirection   scrollDir = ScrollDirection::Natural;
    // ScrollWheel only, as a percentage — see kScrollSpeedDefault.
    uint32_t          scrollSpeed = kScrollSpeedDefault;
    BackButtonBinding click     = BackButtonBinding::FromAction(BackButtonAction::None);
    // Fires while a finger rests on the pad, whatever the pad's mode is doing
    // with movement. Deliberately not zoned like the click: touch and click
    // are different physical events so they cannot collide, and a thumb
    // crosses zones constantly while sliding, which would make a zoned touch
    // flicker.
    BackButtonBinding touch     = BackButtonBinding::FromAction(BackButtonAction::None);

    // DirectionalPad only. Default to the gamepad d-pad so choosing the mode
    // does the obvious thing before anything is rebound.
    BackButtonBinding up        = BackButtonBinding::FromAction(BackButtonAction::DPadUp);
    BackButtonBinding down      = BackButtonBinding::FromAction(BackButtonAction::DPadDown);
    BackButtonBinding left      = BackButtonBinding::FromAction(BackButtonAction::DPadLeft);
    BackButtonBinding right     = BackButtonBinding::FromAction(BackButtonAction::DPadRight);
    DiagonalMode      diagonals = DiagonalMode::EightWay;

    bool operator==(const TrackpadSettings& o) const {
        return mode == o.mode && scrollDir == o.scrollDir && click == o.click
            && scrollSpeed == o.scrollSpeed
            && touch == o.touch && up == o.up && down == o.down
            && left == o.left && right == o.right && diagonals == o.diagonals;
    }
    bool operator!=(const TrackpadSettings& o) const { return !(*this == o); }

    // True when this pad's movement is driving the desktop. The two new modes
    // are not here: they dispatch bindings, which a paddle does too, and none
    // of them steer the pointer.
    bool ClaimedForDesktop() const {
        return mode == TrackpadMode::MousePointer || mode == TrackpadMode::ScrollWheel;
    }

    // True when this pad reaches the game through the virtual DS4's touchpad,
    // which also makes its click the touchpad press rather than a binding.
    bool FeedsDs4Touchpad() const { return mode == TrackpadMode::DS4Touchpad; }

    // True when this pad resolves directions, and therefore when its click is
    // split between the centre and the ring.
    bool IsDirectionalPad() const { return mode == TrackpadMode::DirectionalPad; }

    // The binding this pad's click should dispatch, which is nothing at all
    // while the click belongs to the DS4 touchpad. Reading the bindings
    // through these rather than touching the fields directly is what stops one
    // left over from another mode firing from behind hidden UI.
    //
    // A directional pad narrows this further — only a press that lands inside
    // kPadRingRadius is the click, since one out in the ring is a direction —
    // but that needs the press position, which lives with the resolver rather
    // than here. The caller that has it applies the zone; see TrackpadInput.
    BackButtonBinding EffectiveClick() const {
        return FeedsDs4Touchpad() ? BackButtonBinding{} : click;
    }

    // A pad set to None is not in use at all, and a DS4 touchpad's contact is
    // the touchpad contact — every other mode can carry a touch binding.
    BackButtonBinding EffectiveTouch() const {
        return (mode == TrackpadMode::None || FeedsDs4Touchpad()) ? BackButtonBinding{}
                                                                  : touch;
    }

    // The binding for one direction, or nothing when this pad is not a
    // directional pad at all.
    BackButtonBinding EffectiveDirection(PadDir d) const {
        if (!IsDirectionalPad()) return {};
        switch (d) {
        case DirUp:    return up;
        case DirDown:  return down;
        case DirLeft:  return left;
        case DirRight: return right;
        default:       return {};
        }
    }
};

// Everything one profile controls — the global default profile and each
// per-game override are both this shape.
//
// The two pads deliberately start out different, which is why these defaults
// live here rather than on TrackpadSettings (one struct, one default, no way
// to say "left scrolls, right points"). Out of the box the controller is a
// usable desktop pointing device: right pad moves the cursor and clicks, left
// pad scrolls. "Reset this profile to default" in the UI restores exactly
// this — keep the two in step.
struct ControllerProfile {
    // The game's name as the picker showed it, carried so anything that has
    // to talk about this profile later can name it. Profile ids are exe
    // paths, steam:// URIs and package identifiers — none of them fit in a
    // sentence — and the friendly name is only available while the installed
    // list is in hand, which is when the profile is created. Empty for the
    // default profile, which is not a game.
    //
    // Deliberately absent from operator== below: it labels the profile, it is
    // not one of the settings the profile applies.
    std::wstring displayName;

    // This profile carries no controls of its own and follows the default
    // profile instead. Meaningful only on a per-game profile — the default has
    // nothing above it to follow, and never sets this.
    //
    // Exists because the "off unless a game profile is running" mode made
    // profiles routine rather than rare: most now exist only to turn the
    // controller on for a game, and forking a whole copy of the default's
    // bindings to say that stranded them the moment the default improved.
    // Everything below is still stored while this is set, so unticking it in
    // the UI gives the game back the controls it had.
    //
    // False by default: every profile written before this existed had controls
    // of its own, and reading those back as followers would silently discard
    // them.
    bool useDefaultMappings = false;

    // Which kind of virtual pad the game sees. Unlike everything else here
    // this cannot be changed in place — the ViGEm target is created as one
    // type or the other — so applying a profile that changes it tears the
    // virtual controller down and builds it again, which a running game sees
    // as a controller unplug and replug.
    ControllerPlatform platform = ControllerPlatform::Xbox;
    BackButtonConfig back;
    TrackpadSettings leftPad{
        .mode      = TrackpadMode::ScrollWheel,
        .scrollDir = ScrollDirection::Natural,
        .click     = BackButtonBinding::FromAction(BackButtonAction::None),
    };
    TrackpadSettings rightPad{
        .mode      = TrackpadMode::MousePointer,
        .scrollDir = ScrollDirection::Natural,
        .click     = BackButtonBinding::FromAction(BackButtonAction::LeftMouseButton),
    };

    bool operator==(const ControllerProfile& o) const {
        return useDefaultMappings == o.useDefaultMappings
            && platform == o.platform
            && back.l4 == o.back.l4 && back.l5 == o.back.l5
            && back.r4 == o.back.r4 && back.r5 == o.back.r5
            && leftPad == o.leftPad && rightPad == o.rightPad;
    }
    bool operator!=(const ControllerProfile& o) const { return !(*this == o); }
};
