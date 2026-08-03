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

bool IsGameRunning();
bool IsNoJoySelected();
const wchar_t* Tooltip(SteamStrategy strategy);

} // namespace SteamStrategies
