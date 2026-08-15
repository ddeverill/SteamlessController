#include "LinuxGameLibrary.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string HomeDir() {
    if (const char* h = getenv("HOME")) return h;
    return "";
}

std::vector<std::string> DesktopDirs() {
    std::vector<std::string> dirs{ "/usr/share/applications", "/usr/local/share/applications" };
    if (const char* xdgData = getenv("XDG_DATA_HOME"))
        dirs.push_back(std::string(xdgData) + "/applications");
    else
        dirs.push_back(HomeDir() + "/.local/share/applications");
    return dirs;
}

// Just enough of the .desktop format for a name + launch command: the
// [Desktop Entry] group's Name/Exec/NoDisplay/Hidden keys. Field codes
// (%U, %f, ...) are stripped from Exec since nothing here launches it —
// the string only needs to be a stable id.
struct DesktopEntry { std::string name, exec; bool noDisplay = false, hidden = false; };

DesktopEntry ParseDesktopFile(const std::string& path) {
    DesktopEntry out;
    std::ifstream f(path);
    std::string line;
    bool inMainGroup = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.front() == '[') {
            inMainGroup = (line == "[Desktop Entry]");
            continue;
        }
        if (!inMainGroup) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        if (key == "Name" && out.name.empty()) out.name = value;
        else if (key == "Exec") out.exec = value;
        else if (key == "NoDisplay") out.noDisplay = (value == "true");
        else if (key == "Hidden")    out.hidden    = (value == "true");
    }
    // Strip field codes — everything from the first '%' token onward is
    // launch-time substitution, not part of a stable identity.
    if (auto pct = out.exec.find('%'); pct != std::string::npos) {
        // Trim back to the preceding space so "%U" doesn't leave a dangling char.
        auto space = out.exec.rfind(' ', pct);
        out.exec = out.exec.substr(0, space == std::string::npos ? pct : space);
    }
    return out;
}

}  // namespace

std::vector<InstalledGame> LinuxGameLibrary::EnumerateInstalled() {
    std::vector<InstalledGame> games;

    for (const auto& dir : DesktopDirs()) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (entry.path().extension() != ".desktop") continue;
            DesktopEntry d = ParseDesktopFile(entry.path().string());
            if (d.noDisplay || d.hidden || d.name.empty() || d.exec.empty()) continue;
            games.push_back({ d.name, d.exec });
        }
    }

    // Steam games — a lighter-weight scan than SteamAppLocator's (no
    // libraryfolders.vdf traversal for additional library drives), since
    // this only feeds a human-facing list for `profile create`, not the
    // foreground-to-app-id matching path that has to be exhaustive.
    for (const auto& root : m_paths.SteamRoots()) {
        std::error_code ec;
        const std::string appsDir = root + "/steamapps";
        if (!fs::is_directory(appsDir, ec)) continue;

        for (const auto& entry : fs::directory_iterator(appsDir, ec)) {
            if (ec) break;
            const std::string name = entry.path().filename().string();
            if (name.rfind("appmanifest_", 0) != 0) continue;

            std::ifstream f(entry.path());
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            auto extract = [&](const char* key) -> std::string {
                const std::string needle = std::string("\"") + key + "\"";
                auto pos = text.find(needle);
                if (pos == std::string::npos) return {};
                auto q1 = text.find('"', pos + needle.size());
                if (q1 == std::string::npos) return {};
                auto q2 = text.find('"', q1 + 1);
                if (q2 == std::string::npos) return {};
                return text.substr(q1 + 1, q2 - q1 - 1);
            };
            const std::string appId = extract("appid");
            const std::string gname = extract("name");
            if (appId.empty() || gname.empty()) continue;
            games.push_back({ gname, "steam://rungameid/" + appId });
        }
    }

    std::sort(games.begin(), games.end(), [](const InstalledGame& a, const InstalledGame& b) {
        return a.name < b.name;
    });
    games.erase(std::unique(games.begin(), games.end(), [](const InstalledGame& a, const InstalledGame& b) {
        return a.id == b.id;
    }), games.end());
    return games;
}
