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

// Applies the selected strategy, stops Steam cleanly, and starts it again.
// Steam is always relaunched so the new controller policy takes effect now.
ApplyResult ApplyAndRestart(SteamStrategy strategy);

// Restores the Steam settings captured before the first strategy change,
// removes SteamlessController's own startup/settings entries, and restarts
// Steam if it was running. Used by the uninstaller.
ApplyResult CleanupForUninstall();

bool IsGameRunning();
bool IsNoJoySelected();
const wchar_t* Tooltip(SteamStrategy strategy);
const wchar_t* StatusLabel(SteamStrategy strategy);
const wchar_t* AppliedMessage(SteamStrategy strategy);

} // namespace SteamStrategies
