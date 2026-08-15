#pragma once
#include <string>
#include <vector>

// One installed application, resolvable to a per-game profile. Called a
// "game" for the feature it serves, but nothing here can actually tell a
// game from any other program — filtering is left to the user.
struct InstalledGame {
    std::string name;
    // Opaque stable identity per-game profiles are keyed on. Windows: an exe
    // path, "steam://rungameid/N", or "aumid:...". Linux: an exe path,
    // "steam://rungameid/N", or a .desktop file id.
    std::string id;
};

class IGameLibrary {
public:
    virtual ~IGameLibrary() = default;
    virtual std::vector<InstalledGame> EnumerateInstalled() = 0;
};
