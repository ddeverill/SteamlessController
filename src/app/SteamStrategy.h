#pragma once

#include <string>

enum class SteamStrategy {
    YieldToSteam        = 0,
    ControllerBlacklist = 1,
    NoJoy               = 2,
};

namespace SteamStrategies {

struct ApplyResult {
    bool         applied = false;
    std::wstring message;
};

// Loads the saved choice, or infers it from Steam's current config for
// installations that predate this setting.
SteamStrategy DetectConfigured();

// Applies the selected strategy and restarts Steam when it was running.
// launchIfStopped preserves the debug menu's explicit relaunch behavior while
// allowing the tray on/off switch to leave an already-closed Steam closed.
ApplyResult ApplyAndRestart(SteamStrategy strategy, bool launchIfStopped = true);

// Restores the Steam settings captured before the first strategy change,
// removes SteamlessController's own startup/settings entries, and restarts
// Steam if it was running. Used by the uninstaller.
ApplyResult CleanupForUninstall();

bool IsGameRunning();
bool IsSteamRunning();
bool IsNoJoySelected();
const wchar_t* Tooltip(SteamStrategy strategy);
const wchar_t* StatusLabel(SteamStrategy strategy);
const wchar_t* AppliedMessage(SteamStrategy strategy);

} // namespace SteamStrategies
