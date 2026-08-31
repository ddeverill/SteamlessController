#include "LauncherGames.h"
#include <Windows.h>
#include <shlobj.h>
#include <cwctype>
#include <iterator>

namespace {

// ---------------------------------------------------------------------------
// Small shared helpers
// ---------------------------------------------------------------------------

std::wstring ToLower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// Launchers are inconsistent about which separator they write and whether
// they leave a trailing one; every comparison below assumes neither.
std::wstring NormalizePath(std::wstring path) {
    for (wchar_t& c : path) if (c == L'/') c = L'\\';
    while (!path.empty() && (path.back() == L'\\' || path.back() == L' '))
        path.pop_back();
    return path;
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// "C:\Program Files\Epic Games\Hades" -> "Hades". A launcher that gives us an
// install directory but no title has nothing better to offer, and the folder
// a game installs into is very nearly always its name.
std::wstring LeafName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// The folders a game is installed *inside*, never the install itself. A
// launcher that reported one of these would produce a profile matching every
// application under it, which is worse than having no profile at all.
bool IsTooShallow(const std::wstring& dir) {
    // Held by pointer: REFKNOWNFOLDERID is a reference type, which an array
    // cannot hold.
    static const KNOWNFOLDERID* const kShallowFolders[] = {
        &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_ProgramData,
        &FOLDERID_UserProgramFiles, &FOLDERID_Windows, &FOLDERID_UserProfiles,
        &FOLDERID_LocalAppData, &FOLDERID_RoamingAppData, &FOLDERID_Profile,
    };

    const std::wstring lower = ToLower(dir);
    for (const KNOWNFOLDERID* folder : kShallowFolders) {
        PWSTR known = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*folder, 0, nullptr, &known)) && known) {
            const bool same = ToLower(NormalizePath(known)) == lower;
            CoTaskMemFree(known);
            if (same) return true;
        } else if (known) {
            CoTaskMemFree(known);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

// Both views, always. GOG, Ubisoft and Epic all install as 32-bit
// applications and write to the WOW6432Node side of a 64-bit machine, but
// hardcoding that path is the wrong fix — it breaks on a 32-bit OS and says
// nothing about which view a future version will pick. Asking for each view
// explicitly and merging leaves the redirection to Windows.
constexpr REGSAM kViews[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };

std::wstring ReadSz(HKEY key, const wchar_t* name) {
    // Long enough for any install path a launcher writes; a value that would
    // not fit is not one worth guessing at.
    wchar_t buf[1024] = {};
    DWORD   size = sizeof(buf);
    DWORD   type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf), &size) != ERROR_SUCCESS)
        return {};
    if (type != REG_SZ && type != REG_EXPAND_SZ) return {};
    buf[ARRAYSIZE(buf) - 1] = L'\0';  // RegQueryValueEx does not promise one
    return buf;
}

std::vector<std::wstring> SubKeyNames(HKEY key) {
    std::vector<std::wstring> names;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD   len = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr)
            != ERROR_SUCCESS)
            break;
        names.emplace_back(name, len);
    }
    return names;
}

// Calls visit(childKey) for every subkey of path, in both registry views. A
// game present in both views is visited twice; that costs nothing, because
// both visits produce the same install directory and therefore the same id,
// which GameLibrary's dedup collapses.
template <typename Fn>
void ForEachSubKey(HKEY root, const wchar_t* path, Fn visit) {
    for (REGSAM view : kViews) {
        HKEY parent = nullptr;
        if (RegOpenKeyExW(root, path, 0, KEY_READ | view, &parent) != ERROR_SUCCESS)
            continue;
        for (const auto& name : SubKeyNames(parent)) {
            HKEY child = nullptr;
            if (RegOpenKeyExW(parent, name.c_str(), 0, KEY_READ | view, &child)
                == ERROR_SUCCESS) {
                visit(child);
                RegCloseKey(child);
            }
        }
        RegCloseKey(parent);
    }
}

// ---------------------------------------------------------------------------
// A very small JSON reader, for Epic's manifests
// ---------------------------------------------------------------------------
//
// Four fields out of one flat object does not justify taking on a JSON
// dependency, and the hand-rolled reader in RemapWindow.cpp cannot be reused:
// it deliberately unescapes nothing, because the channel it serves is ASCII
// by construction. These files are UTF-8 and full of escaped backslashes
// (every path) and occasionally \uXXXX (an accented title), so both have to
// be handled. Written in the same spirit as SteamAppLocator.cpp's VdfScanner.

std::wstring ReadFileUtf8(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > (4 << 20)) {
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

int HexDigit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// Reads a JSON string body starting just past its opening quote, leaving pos
// just past the closing one.
std::wstring ReadJsonString(const std::wstring& text, size_t& pos) {
    std::wstring out;
    while (pos < text.size() && text[pos] != L'"') {
        if (text[pos] != L'\\') { out += text[pos++]; continue; }
        if (++pos >= text.size()) break;
        const wchar_t esc = text[pos++];
        switch (esc) {
            case L'n': out += L'\n'; break;
            case L'r': out += L'\r'; break;
            case L't': out += L'\t'; break;
            case L'b': out += L'\b'; break;
            case L'f': out += L'\f'; break;
            case L'u': {
                // A surrogate pair arrives as two consecutive escapes and
                // needs no special handling: each half is a valid wchar_t, so
                // copying both through reproduces the original UTF-16.
                if (pos + 4 > text.size()) return out;
                int code = 0;
                for (int i = 0; i < 4; ++i) {
                    const int digit = HexDigit(text[pos + static_cast<size_t>(i)]);
                    if (digit < 0) return out;
                    code = code * 16 + digit;
                }
                pos += 4;
                out += static_cast<wchar_t>(code);
                break;
            }
            default: out += esc; break;  // covers \" \\ \/
        }
    }
    if (pos < text.size()) ++pos;  // closing quote
    return out;
}

// The offset just past `"<key>":`, or npos. Every field this needs is at the
// top level of the object and no nested key shares a name with one of them,
// so a plain scan is enough — building a parse tree would be machinery in
// service of nothing.
size_t FindKey(const std::wstring& text, const wchar_t* key) {
    const std::wstring needle = std::wstring(L"\"") + key + L"\"";
    size_t pos = text.find(needle);
    if (pos == std::wstring::npos) return std::wstring::npos;
    pos += needle.size();
    while (pos < text.size() && iswspace(text[pos])) ++pos;
    if (pos >= text.size() || text[pos] != L':') return std::wstring::npos;
    ++pos;
    while (pos < text.size() && iswspace(text[pos])) ++pos;
    return pos;
}

std::wstring JsonString(const std::wstring& text, const wchar_t* key) {
    size_t pos = FindKey(text, key);
    if (pos == std::wstring::npos || pos >= text.size() || text[pos] != L'"')
        return {};
    ++pos;
    return ReadJsonString(text, pos);
}

// Absent reads as the default, so a manifest written before Epic added a flag
// keeps whatever behaviour it had.
bool JsonBool(const std::wstring& text, const wchar_t* key, bool def) {
    const size_t pos = FindKey(text, key);
    if (pos == std::wstring::npos) return def;
    if (text.compare(pos, 4, L"true")  == 0) return true;
    if (text.compare(pos, 5, L"false") == 0) return false;
    return def;
}

// ---------------------------------------------------------------------------

// The one place an entry is built, so every source gets the same validation:
// a directory that does not exist, or is too broad to identify anything, is
// dropped rather than turned into a profile that misfires.
void AddIfUsable(std::vector<InstalledGame>& out, std::wstring name,
                 const std::wstring& installDir, GameSource source) {
    const std::wstring dir = NormalizePath(installDir);
    if (!LauncherGames::IsUsableInstallDir(dir)) return;
    if (name.empty()) name = LeafName(dir);
    if (name.empty()) return;
    out.push_back({ std::move(name), L"dir:" + dir, source });
}

void Append(std::vector<InstalledGame>& into, std::vector<InstalledGame> found) {
    into.insert(into.end(), std::make_move_iterator(found.begin()),
                std::make_move_iterator(found.end()));
}

}  // namespace

namespace LauncherGames {

bool IsUsableInstallDir(const std::wstring& dir) {
    if (dir.size() < 4) return false;  // shorter than "C:\x" identifies nothing
    // A drive root survives NormalizePath as "C:" and a UNC share root as
    // "\\server\share"; neither names an install.
    if (dir.find(L'\\') == std::wstring::npos) return false;
    if (IsTooShallow(dir)) return false;
    return DirectoryExists(dir);
}

// ---------------------------------------------------------------------------
// Epic Games Store
// ---------------------------------------------------------------------------

std::vector<InstalledGame> EnumerateEpic(const std::wstring& manifestsDir) {
    std::vector<InstalledGame> games;

    std::wstring dir = NormalizePath(manifestsDir);
    if (dir.empty()) {
        // The launcher's own record of where it put them wins, because a user
        // can move this. The fixed path under ProgramData below is where it
        // lands by default, and the only answer left on a machine whose
        // registry entry has gone missing.
        for (REGSAM view : kViews) {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                              L"SOFTWARE\\Epic Games\\EpicGamesLauncher",
                              0, KEY_READ | view, &key) != ERROR_SUCCESS)
                continue;
            const std::wstring appData = ReadSz(key, L"AppDataPath");
            RegCloseKey(key);
            if (!appData.empty()) {
                dir = NormalizePath(appData) + L"\\Manifests";
                break;
            }
        }
    }
    if (dir.empty()) {
        PWSTR programData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData))
            && programData)
            dir = NormalizePath(programData) + L"\\Epic\\EpicGamesLauncher\\Data\\Manifests";
        if (programData) CoTaskMemFree(programData);
    }
    if (dir.empty()) return games;

    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*.item").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return games;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        const std::wstring text = ReadFileUtf8(dir + L"\\" + fd.cFileName);
        if (text.empty()) continue;

        // Epic writes a manifest for every installed *item*, which includes
        // DLC, language packs and engine plugins. bIsApplication is what
        // separates the things a user could put in the foreground from the
        // ones that only ride along inside another.
        if (!JsonBool(text, L"bIsApplication", true)) continue;
        // A download that stopped part-way still has a manifest and a
        // directory, but not a game.
        if (JsonBool(text, L"bIsIncompleteInstall", false)) continue;

        AddIfUsable(games, JsonString(text, L"DisplayName"),
                    JsonString(text, L"InstallLocation"), GameSource::Epic);
    } while (FindNextFileW(find, &fd));

    FindClose(find);
    return games;
}

// ---------------------------------------------------------------------------
// GOG Galaxy
// ---------------------------------------------------------------------------

std::vector<InstalledGame> EnumerateGog() {
    std::vector<InstalledGame> games;
    ForEachSubKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games", [&games](HKEY key) {
        // "gameName" is the title as GOG shows it, "path" is where it went.
        // Both are written by the installer for every game, so a subkey
        // missing either is not one to guess about.
        AddIfUsable(games, ReadSz(key, L"gameName"), ReadSz(key, L"path"),
                    GameSource::Gog);
    });
    return games;
}

// ---------------------------------------------------------------------------
// Ubisoft Connect
// ---------------------------------------------------------------------------

std::vector<InstalledGame> EnumerateUbisoft() {
    std::vector<InstalledGame> games;
    ForEachSubKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Ubisoft\\Launcher\\Installs",
                  [&games](HKEY key) {
        // The subkey name is a Ubisoft product id and the key holds no title,
        // so the name has to come from the install folder — AddIfUsable falls
        // back to it when handed nothing. InstallDir is written with forward
        // slashes, which NormalizePath deals with.
        AddIfUsable(games, {}, ReadSz(key, L"InstallDir"), GameSource::Ubisoft);
    });
    return games;
}

// ---------------------------------------------------------------------------
// Battle.net
// ---------------------------------------------------------------------------

std::vector<InstalledGame> EnumerateBattleNet() {
    std::vector<InstalledGame> games;

    // Blizzard's own catalogue is a protobuf dump (product.db) that would
    // need a decoder to read. The uninstall entries Battle.net writes carry
    // the same two facts and are plain registry values — the same trade
    // Playnite's Battle.net library makes, which keeps product.db only as a
    // fallback for games imported rather than installed.
    static constexpr wchar_t kUninstallPath[] =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

    for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER }) {
        ForEachSubKey(root, kUninstallPath, [&games](HKEY key) {
            // Battle.net uninstalls run through the launcher itself, as
            // "...Battle.net.exe --uninstall --uid=<product> ...". Both
            // markers together are what separates a Blizzard game from the
            // hundred other things in this part of the registry, which is far
            // too noisy to read wholesale.
            const std::wstring lower  = ToLower(ReadSz(key, L"UninstallString"));
            const size_t       uidPos = lower.find(L"--uid=");
            if (uidPos == std::wstring::npos
                || lower.find(L"battle.net") == std::wstring::npos)
                return;

            size_t uidEnd = lower.find_first_of(L" \"", uidPos);
            if (uidEnd == std::wstring::npos) uidEnd = lower.size();
            const std::wstring uid = lower.substr(uidPos + 6, uidEnd - uidPos - 6);

            // The launcher registers itself the same way its games do, so
            // without this the client shows up in the picker as though it
            // were one of them — which is how it first turned up on the
            // machine this was written on, Battle.net installed and no
            // Blizzard game on it at all.
            for (const wchar_t* client : { L"battle.net", L"bna", L"agent" })
                if (uid == client) return;

            // Public test and beta clients install alongside the real game
            // and share its name; a profile for one would be a surprise.
            for (const wchar_t* suffix : { L"test", L"beta", L"ptr" }) {
                const size_t len = wcslen(suffix);
                if (uid.size() > len && uid.compare(uid.size() - len, len, suffix) == 0)
                    return;
            }

            AddIfUsable(games, ReadSz(key, L"DisplayName"),
                        ReadSz(key, L"InstallLocation"), GameSource::BattleNet);
        });
    }
    return games;
}

// ---------------------------------------------------------------------------

std::vector<InstalledGame> EnumerateAll() {
    std::vector<InstalledGame> games;
    Append(games, EnumerateEpic());
    Append(games, EnumerateGog());
    Append(games, EnumerateUbisoft());
    Append(games, EnumerateBattleNet());
    return games;
}

}  // namespace LauncherGames
