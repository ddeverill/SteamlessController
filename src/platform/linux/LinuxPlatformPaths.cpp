#include "LinuxPlatformPaths.h"
#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

namespace {

std::string HomeDir() {
    if (const char* home = getenv("HOME"); home && *home) return home;
    if (const struct passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return "";
}

std::string XdgDir(const char* envVar, const char* fallbackUnderHome) {
    if (const char* v = getenv(envVar); v && *v) return v;
    return HomeDir() + fallbackUnderHome;
}

}  // namespace

std::string LinuxPlatformPaths::StateDir() const {
    return XdgDir("XDG_STATE_HOME", "/.local/state") + "/steamlesscontroller";
}

std::string LinuxPlatformPaths::ConfigDir() const {
    return XdgDir("XDG_CONFIG_HOME", "/.config") + "/steamlesscontroller";
}

std::vector<std::string> LinuxPlatformPaths::SteamRoots() const {
    const std::string home = HomeDir();
    const std::string dataHome = XdgDir("XDG_DATA_HOME", "/.local/share");
    return {
        dataHome + "/Steam",
        home + "/.steam/steam",
        home + "/.steam/root",
        home + "/.steam/debian-installation",
        home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam",  // Flatpak
        home + "/snap/steam/common/.local/share/Steam",                  // Snap
    };
}
