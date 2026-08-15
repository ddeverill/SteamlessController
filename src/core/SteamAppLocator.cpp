#include "SteamAppLocator.h"
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

// Case-insensitive compare for VDF key names, which are always ASCII
// ("path", "appid", "installdir", "StateFlags") regardless of platform.
bool IEquals(const std::string& a, const char* b) {
    size_t i = 0;
    for (; a[i] && b[i]; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return a[i] == '\0' && b[i] == '\0';
}

// Steam's manifests are all the same shape: a quoted key followed by either a
// quoted value or a brace-delimited block. That is little enough grammar to
// read directly, and the alternative — a general VDF library — would be far
// more machinery than two files' worth of key lookups justifies.
//
// Only the tokens matter here, not the tree: every value this needs is
// identified by its key plus how deeply nested it is, so the scanner tracks a
// depth counter instead of building nodes. Operates directly on the file's
// UTF-8 bytes — every structural character here (quote, brace, backslash) is
// ASCII, and ASCII bytes never appear as part of a multi-byte UTF-8 sequence,
// so byte-wise scanning is safe without decoding.
class VdfScanner {
public:
    explicit VdfScanner(const std::string& text) : m_text(text) {}

    struct Token {
        enum class Kind { String, OpenBrace, CloseBrace, End } kind = Kind::End;
        std::string value;
    };

    Token Next() {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == '{') { ++m_pos; return { Token::Kind::OpenBrace,  {} }; }
            if (c == '}') { ++m_pos; return { Token::Kind::CloseBrace, {} }; }
            if (c == '"') { ++m_pos; return { Token::Kind::String, ReadQuoted() }; }
            // Whitespace between tokens, and the odd comment line Steam
            // writes into these files.
            if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '/') {
                while (m_pos < m_text.size() && m_text[m_pos] != '\n') ++m_pos;
                continue;
            }
            ++m_pos;
        }
        return {};
    }

private:
    std::string ReadQuoted() {
        std::string out;
        while (m_pos < m_text.size() && m_text[m_pos] != '"') {
            // Paths are written with their separators escaped, so an
            // unprocessed value would be full of doubled backslashes.
            if (m_text[m_pos] == '\\' && m_pos + 1 < m_text.size()) {
                ++m_pos;
                out += m_text[m_pos++];
                continue;
            }
            out += m_text[m_pos++];
        }
        if (m_pos < m_text.size()) ++m_pos;  // closing quote
        return out;
    }

    const std::string& m_text;
    size_t             m_pos = 0;
};

std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const auto size = f.tellg();
    if (size <= 0 || size > (8 << 20)) return {};
    std::string bytes(static_cast<size_t>(size), '\0');
    f.seekg(0);
    f.read(bytes.data(), size);
    return bytes;
}

// Every library root Steam knows about, the default install included.
std::vector<std::string> LibraryRoots(const std::string& steamPath) {
    std::vector<std::string> roots{ steamPath };

    const std::string text = ReadFile(steamPath + "/steamapps/libraryfolders.vdf");
    if (text.empty()) return roots;

    VdfScanner scanner(text);
    int         depth = 0;
    std::string pendingKey;
    for (;;) {
        const auto tok = scanner.Next();
        if (tok.kind == VdfScanner::Token::Kind::End) break;
        if (tok.kind == VdfScanner::Token::Kind::OpenBrace)  { ++depth; pendingKey.clear(); continue; }
        if (tok.kind == VdfScanner::Token::Kind::CloseBrace) { --depth; pendingKey.clear(); continue; }

        if (pendingKey.empty()) { pendingKey = tok.value; continue; }
        // "libraryfolders" { "0" { "path" "..." } } — depth 2 is where a
        // library's own fields live. Anchoring on the depth keeps a "path"
        // appearing anywhere else in the file from being mistaken for one.
        if (depth == 2 && IEquals(pendingKey, "path") && !tok.value.empty())
            roots.push_back(tok.value);
        pendingKey.clear();
    }
    return roots;
}

// StateFlags is a bitfield; this is the only bit that means the game is
// actually on disk and playable. Without the check a half-downloaded or
// part-uninstalled app claims an install directory that may not exist.
constexpr int kStateFullyInstalled = 4;

}  // namespace

std::string SteamAppLocator::FoldKey(std::string s) const {
    if (m_caseInsensitive)
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string SteamAppLocator::NormalizeDir(std::string dir) const {
    dir = FoldKey(std::move(dir));
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    return dir;
}

void SteamAppLocator::Refresh() {
    m_installDirs.clear();

    for (const auto& steamPath : m_roots) {
        if (steamPath.empty()) continue;

        for (const auto& root : LibraryRoots(steamPath)) {
            const std::string appsDir = root + "/steamapps";
            std::error_code ec;
            if (!fs::is_directory(appsDir, ec)) continue;

            for (const auto& entry : fs::directory_iterator(appsDir, ec)) {
                if (ec) break;
                if (!entry.is_regular_file()) continue;
                const std::string name = entry.path().filename().string();
                if (name.rfind("appmanifest_", 0) != 0) continue;
                if (name.size() < 4 || name.compare(name.size() - 4, 4, ".acf") != 0) continue;

                const std::string text = ReadFile(entry.path().string());
                if (text.empty()) continue;

                std::string appId, installDir;
                int         stateFlags = 0;

                VdfScanner scanner(text);
                int         depth = 0;
                std::string pendingKey;
                for (;;) {
                    const auto tok = scanner.Next();
                    if (tok.kind == VdfScanner::Token::Kind::End) break;
                    if (tok.kind == VdfScanner::Token::Kind::OpenBrace)  { ++depth; pendingKey.clear(); continue; }
                    if (tok.kind == VdfScanner::Token::Kind::CloseBrace) { --depth; pendingKey.clear(); continue; }

                    if (pendingKey.empty()) { pendingKey = tok.value; continue; }
                    // "AppState" { "appid" "..." "installdir" "..." } — depth 1.
                    if (depth == 1) {
                        if      (IEquals(pendingKey, "appid"))      appId      = tok.value;
                        else if (IEquals(pendingKey, "installdir")) installDir = tok.value;
                        else if (IEquals(pendingKey, "StateFlags"))
                            stateFlags = std::atoi(tok.value.c_str());
                    }
                    pendingKey.clear();
                }

                if (appId.empty() || installDir.empty()) continue;
                if (!(stateFlags & kStateFullyInstalled)) continue;

                const std::string full = appsDir + "/common/" + installDir;
                if (!fs::exists(full, ec)) continue;

                // First library wins. A game present in two libraries is Steam's
                // problem to reconcile, and either answer names the same game.
                m_installDirs.emplace(appId, NormalizeDir(full));
            }
        }
    }
}

std::string SteamAppLocator::AppIdForPath(const std::string& exePath) const {
    if (exePath.empty() || m_installDirs.empty()) return {};

    const std::string key = FoldKey(exePath);

    std::string bestId;
    size_t      bestLen = 0;
    for (const auto& [appId, dir] : m_installDirs) {
        if (dir.size() <= bestLen) continue;          // cannot beat what we have
        if (key.compare(0, dir.size(), dir) != 0) continue;
        bestId  = appId;
        bestLen = dir.size();
    }
    return bestId;
}
