#include "SteamAppLocator.h"
#include <Windows.h>
#include <cwctype>

namespace {

// Steam's manifests are all the same shape: a quoted key followed by either a
// quoted value or a brace-delimited block. That is little enough grammar to
// read directly, and the alternative — a general VDF library — would be far
// more machinery than two files' worth of key lookups justifies.
//
// Only the tokens matter here, not the tree: every value this needs is
// identified by its key plus how deeply nested it is, so the scanner tracks a
// depth counter instead of building nodes.
class VdfScanner {
public:
    explicit VdfScanner(const std::wstring& text) : m_text(text) {}

    struct Token {
        enum class Kind { String, OpenBrace, CloseBrace, End } kind = Kind::End;
        std::wstring value;
    };

    Token Next() {
        while (m_pos < m_text.size()) {
            const wchar_t c = m_text[m_pos];
            if (c == L'{') { ++m_pos; return { Token::Kind::OpenBrace,  {} }; }
            if (c == L'}') { ++m_pos; return { Token::Kind::CloseBrace, {} }; }
            if (c == L'"') { ++m_pos; return { Token::Kind::String, ReadQuoted() }; }
            // Whitespace between tokens, and the odd comment line Steam
            // writes into these files.
            if (c == L'/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == L'/') {
                while (m_pos < m_text.size() && m_text[m_pos] != L'\n') ++m_pos;
                continue;
            }
            ++m_pos;
        }
        return {};
    }

private:
    std::wstring ReadQuoted() {
        std::wstring out;
        while (m_pos < m_text.size() && m_text[m_pos] != L'"') {
            // Paths are written with their separators escaped, so an
            // unprocessed value would be full of doubled backslashes.
            if (m_text[m_pos] == L'\\' && m_pos + 1 < m_text.size()) {
                ++m_pos;
                out += m_text[m_pos++];
                continue;
            }
            out += m_text[m_pos++];
        }
        if (m_pos < m_text.size()) ++m_pos;  // closing quote
        return out;
    }

    const std::wstring& m_text;
    size_t              m_pos = 0;
};

std::wstring ReadUtf8File(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > (8 << 20)) {
        CloseHandle(f);
        return {};
    }

    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()),
                             &read, nullptr) != FALSE;
    CloseHandle(f);
    if (!ok) return {};
    bytes.resize(read);

    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                      static_cast<int>(bytes.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring text(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                        text.data(), n);
    return text;
}

std::wstring SteamInstallPath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD   size = sizeof(buf);
    // Written by the client for the current user; the machine-wide key exists
    // too but points at the installer's idea of the path, not the user's.
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    std::wstring path(buf);
    // Steam stores this with forward slashes and in lower case.
    for (wchar_t& c : path) if (c == L'/') c = L'\\';
    while (!path.empty() && path.back() == L'\\') path.pop_back();
    return path;
}

std::wstring NormalizeDir(std::wstring dir) {
    for (wchar_t& c : dir) c = static_cast<wchar_t>(towlower(c));
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    return dir;
}

// Every library root Steam knows about, the default install included.
std::vector<std::wstring> LibraryRoots(const std::wstring& steamPath) {
    std::vector<std::wstring> roots{ steamPath };

    const std::wstring text = ReadUtf8File(steamPath + L"\\steamapps\\libraryfolders.vdf");
    if (text.empty()) return roots;

    VdfScanner scanner(text);
    int          depth      = 0;
    std::wstring pendingKey;
    for (;;) {
        const auto tok = scanner.Next();
        if (tok.kind == VdfScanner::Token::Kind::End) break;
        if (tok.kind == VdfScanner::Token::Kind::OpenBrace)  { ++depth; pendingKey.clear(); continue; }
        if (tok.kind == VdfScanner::Token::Kind::CloseBrace) { --depth; pendingKey.clear(); continue; }

        if (pendingKey.empty()) { pendingKey = tok.value; continue; }
        // "libraryfolders" { "0" { "path" "..." } } — depth 2 is where a
        // library's own fields live. Anchoring on the depth keeps a "path"
        // appearing anywhere else in the file from being mistaken for one.
        if (depth == 2 && _wcsicmp(pendingKey.c_str(), L"path") == 0 && !tok.value.empty())
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

void SteamAppLocator::Refresh() {
    m_installDirs.clear();

    const std::wstring steamPath = SteamInstallPath();
    if (steamPath.empty()) return;

    for (const auto& root : LibraryRoots(steamPath)) {
        const std::wstring appsDir = root + L"\\steamapps";

        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((appsDir + L"\\appmanifest_*.acf").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) continue;

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            const std::wstring text = ReadUtf8File(appsDir + L"\\" + fd.cFileName);
            if (text.empty()) continue;

            std::wstring appId, installDir;
            int          stateFlags = 0;

            VdfScanner scanner(text);
            int          depth = 0;
            std::wstring pendingKey;
            for (;;) {
                const auto tok = scanner.Next();
                if (tok.kind == VdfScanner::Token::Kind::End) break;
                if (tok.kind == VdfScanner::Token::Kind::OpenBrace)  { ++depth; pendingKey.clear(); continue; }
                if (tok.kind == VdfScanner::Token::Kind::CloseBrace) { --depth; pendingKey.clear(); continue; }

                if (pendingKey.empty()) { pendingKey = tok.value; continue; }
                // "AppState" { "appid" "..." "installdir" "..." } — depth 1.
                if (depth == 1) {
                    if      (_wcsicmp(pendingKey.c_str(), L"appid")      == 0) appId      = tok.value;
                    else if (_wcsicmp(pendingKey.c_str(), L"installdir") == 0) installDir = tok.value;
                    else if (_wcsicmp(pendingKey.c_str(), L"StateFlags") == 0)
                        stateFlags = _wtoi(tok.value.c_str());
                }
                pendingKey.clear();
            }

            if (appId.empty() || installDir.empty()) continue;
            if (!(stateFlags & kStateFullyInstalled)) continue;

            const std::wstring full = appsDir + L"\\common\\" + installDir;
            if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) continue;

            // First library wins. A game present in two libraries is Steam's
            // problem to reconcile, and either answer names the same game.
            m_installDirs.emplace(appId, NormalizeDir(full));
        } while (FindNextFileW(find, &fd));

        FindClose(find);
    }
}

std::wstring SteamAppLocator::AppIdForPath(const std::wstring& exePath) const {
    if (exePath.empty() || m_installDirs.empty()) return {};

    std::wstring lower = exePath;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));

    std::wstring bestId;
    size_t       bestLen = 0;
    for (const auto& [appId, dir] : m_installDirs) {
        if (dir.size() <= bestLen) continue;          // cannot beat what we have
        if (lower.compare(0, dir.size(), dir) != 0) continue;
        bestId  = appId;
        bestLen = dir.size();
    }
    return bestId;
}
