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
    COUNT
};

inline const char* TrackpadModeId(TrackpadMode m) {
    switch (m) {
    case TrackpadMode::MousePointer: return "pointer";
    case TrackpadMode::ScrollWheel:  return "scroll";
    case TrackpadMode::DS4Touchpad:  return "ds4";
    default:                         return "none";
    }
}

inline TrackpadMode TrackpadModeFromId(const std::string& id) {
    if (id == "pointer") return TrackpadMode::MousePointer;
    if (id == "scroll")  return TrackpadMode::ScrollWheel;
    if (id == "ds4")     return TrackpadMode::DS4Touchpad;
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

// One physical trackpad's configuration. The click is a BackButtonBinding
// rather than a type of its own: a pad click and a back paddle are the same
// question ("what should this button do"), answered by the same catalog and
// dispatched down the same path.
struct TrackpadSettings {
    TrackpadMode      mode      = TrackpadMode::None;
    ScrollDirection   scrollDir = ScrollDirection::Natural;
    BackButtonBinding click     = BackButtonBinding::FromAction(BackButtonAction::None);

    bool operator==(const TrackpadSettings& o) const {
        return mode == o.mode && scrollDir == o.scrollDir && click == o.click;
    }
    bool operator!=(const TrackpadSettings& o) const { return !(*this == o); }

    // True when this pad's movement is driving the desktop.
    bool ClaimedForDesktop() const {
        return mode == TrackpadMode::MousePointer || mode == TrackpadMode::ScrollWheel;
    }

    // True when this pad reaches the game through the virtual DS4's touchpad,
    // which also makes its click the touchpad press rather than a binding.
    bool FeedsDs4Touchpad() const { return mode == TrackpadMode::DS4Touchpad; }

    // The binding this pad's click should dispatch, which is nothing at all
    // while the click belongs to the DS4 touchpad. Reading it through here
    // rather than touching `click` directly is what stops a binding left over
    // from an earlier mode firing behind the hidden UI.
    BackButtonBinding EffectiveClick() const {
        return FeedsDs4Touchpad() ? BackButtonBinding{} : click;
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
    std::string displayName;

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
