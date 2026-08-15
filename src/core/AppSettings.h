#pragma once
#include <cstdint>

// When the controller yields itself back to Steam automatically. Mirrors
// the tray app's "Auto Steam Mode" tri-state.
enum class AutoMode : uint8_t {
    Manual        = 0,  // never auto-yield; only the explicit acquire/release toggle matters
    OffWhileSteam = 1,  // yield whenever Steam is running at all
    OffOnlyInGame = 2,  // yield only while a Steam game is actually running
};

struct AppSettings {
    AutoMode autoMode      = AutoMode::Manual;
    bool     notifications = true;
};
