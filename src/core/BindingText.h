#pragma once
#include <optional>
#include <string>
#include "BackButtonConfig.h"

// Human-friendly binding syntax shared by steamlessctl, the config file, and
// (for display) the Windows remap UI — see the CLI's `actions` command for
// the full catalog this accepts.
//
// Grammar: a bare action id (BackButtonActionId — "A", "leftMouse", "none",
// ...), "mouse:middle"/"mouse:x1"/"mouse:x2", the legacy packed numeric form
// "key:<n>" (BackButtonBinding::Id()'s own wire format, always accepted for
// backward compatibility), or the readable form
// "key:[ctrl+][alt+][shift+][win+]<KeyName>" where <KeyName> is a
// KeyboardEvent.code name (KeyIdFromJsCode) — e.g. "key:ctrl+alt+KeyC".
namespace BindingText {

// Returns nullopt for text that doesn't parse as any recognised form.
std::optional<BackButtonBinding> Parse(const std::string& text);

// The readable spelling, round-tripping through Parse(). Keys are named via
// the shared KeyNames catalog (JsCodeFromKeyId) — physical-position names
// like "KeyC", not a per-layout display name — falling back to the numeric
// packed wire form for a key id the catalog has no name for, so the string
// still round-trips either way. This is what SettingsCodec writes into a
// PrefersText() (Linux/TOML) store instead of BackButtonBinding::Id()'s bare
// packed number — the whole reason a hand-edited config file reads as
// "key:ctrl+KeyC" rather than "key:65603".
std::string Format(const BackButtonBinding& b);

}  // namespace BindingText
