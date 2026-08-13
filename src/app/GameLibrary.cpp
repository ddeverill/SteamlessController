#define NOMINMAX  // avoid Windows.h's min/max macros colliding with C++/WinRT templates
#include "GameLibrary.h"
#include <Windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Management.Deployment.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <unordered_set>
#pragma comment(lib, "shlwapi.lib")

namespace {

// Targets under the Windows directory are system utilities (Notepad, Control
// Panel applets, accessories) that Start Menu shortcuts surface right
// alongside real applications — filtered out so the picker isn't dominated
// by them.
bool IsUnderWindowsDir(const std::wstring& path) {
    wchar_t winDir[MAX_PATH];
    if (!GetWindowsDirectoryW(winDir, MAX_PATH)) return false;
    const size_t len = wcslen(winDir);
    return path.size() > len
        && _wcsnicmp(path.c_str(), winDir, len) == 0
        && (path[len] == L'\\' || path[len] == L'\0');
}

// Shortcut names that are never the game/app itself, just noise that rides
// along with it in the same Start Menu folder — "Uninstall X", "X Readme".
// Only applied to .lnk-sourced entries: a real name can contain one of these
// words as part of a longer word ("Truck-kun is Supporting Me..." contains
// "support"), so matching is whole-word, not substring.
bool LooksLikeNoise(const std::wstring& name) {
    static const wchar_t* kNoisePhrases[] = { L"read me" };  // has a space; safe as a substring
    static const wchar_t* kNoiseWords[] = {
        L"uninstall", L"readme", L"license", L"changelog",
        L"help", L"documentation", L"website", L"support", L"faq",
    };

    std::wstring lower = name;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));

    for (const wchar_t* phrase : kNoisePhrases)
        if (lower.find(phrase) != std::wstring::npos) return true;

    size_t start = 0;
    while (start < lower.size()) {
        size_t end = start;
        while (end < lower.size() && iswalnum(lower[end])) ++end;
        if (end > start) {
            const std::wstring word = lower.substr(start, end - start);
            for (const wchar_t* noise : kNoiseWords)
                if (word == noise) return true;
        }
        start = end + 1;
    }
    return false;
}

// Case-insensitive identity for dedup. Only a bare filesystem path benefits
// from PathCanonicalizeW (collapsing "." / ".." segments); running it over a
// "steam://" URI or an "aumid:" id risks mangling them for no reason, since
// neither one follows filesystem path rules.
std::wstring DedupKey(const std::wstring& id) {
    std::wstring key;
    if (id.rfind(L"steam://", 0) == 0 || id.rfind(L"aumid:", 0) == 0) {
        key = id;
    } else {
        wchar_t buf[MAX_PATH];
        key = PathCanonicalizeW(buf, id.c_str()) ? buf : id;
    }
    for (wchar_t& c : key) c = static_cast<wchar_t>(towlower(c));
    return key;
}

// A file extension worth resolving further — anything else in the Start
// Menu (icons, config, whatever an installer left behind) is not a shortcut.
bool HasExtension(const std::wstring& path, const wchar_t* ext) {
    const size_t dot = path.find_last_of(L'.');
    return dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, ext) == 0;
}

void WalkDirectory(const std::wstring& dir, std::vector<std::wstring>& shortcutPaths) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        const std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            WalkDirectory(full, shortcutPaths);
        } else if (HasExtension(full, L".lnk") || HasExtension(full, L".url")) {
            shortcutPaths.push_back(full);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

// Empty when the shortcut has no literal file target — the case for a
// packaged/UWP app, which resolves through an AUMID instead of a path.
std::wstring ResolveLnkTarget(IShellLinkW* link) {
    wchar_t target[MAX_PATH] = {};
    if (FAILED(link->GetPath(target, MAX_PATH, nullptr, 0)) || target[0] == L'\0')
        return {};
    return target;
}

// Every Steam-installed game gets one of these instead of a .lnk — an
// Internet Shortcut whose URL is "steam://rungameid/<appid>". Anything else
// under a "URL=" line (Steam also drops a couple of plain website links in
// the same folder, e.g. its support center) is not a game and is skipped.
// .url files are plain INI-style text, always ASCII in the one line this
// cares about, so a byte-for-byte narrow read is safe.
std::wstring ResolveUrlSteamId(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) return {};

    static constexpr char kLinePrefix[]   = "URL=";
    static constexpr size_t kLinePrefixLen = sizeof(kLinePrefix) - 1;
    static constexpr char kUriPrefix[]    = "steam://rungameid/";
    static constexpr size_t kUriPrefixLen = sizeof(kUriPrefix) - 1;

    char line[512];
    std::string uri;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, kLinePrefix, kLinePrefixLen) != 0) continue;
        const char* value = line + kLinePrefixLen;
        if (strncmp(value, kUriPrefix, kUriPrefixLen) != 0) continue;
        uri = value;  // keep the steam:// URI itself, "URL=" already dropped
        break;
    }
    fclose(f);

    while (!uri.empty() && (uri.back() == '\n' || uri.back() == '\r'))
        uri.pop_back();
    if (uri.size() <= kUriPrefixLen) return {};
    // The appid must be purely numeric — anything else means this line was
    // not the simple form assumed above, and guessing further isn't worth it.
    for (size_t i = kUriPrefixLen; i < uri.size(); ++i)
        if (uri[i] < '0' || uri[i] > '9') return {};

    return std::wstring(uri.begin(), uri.end());
}

std::wstring NameFromShortcutPath(const std::wstring& shortcutPath) {
    std::wstring name = shortcutPath;
    const size_t slash = name.find_last_of(L'\\');
    if (slash != std::wstring::npos) name.erase(0, slash + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.erase(dot);
    return name;
}

// Packaged (MSIX/UWP) apps — the Xbox app's Game Pass titles among them —
// register no shortcut file the Start Menu walk above can see; Windows
// builds their Start Menu presence straight from the package manifest.
// Resolved through the real WinRT PackageManager API rather than the
// simpler shell:AppsFolder namespace specifically because only PackageManager
// exposes each package's signing info — Playnite's own Xbox library extension
// filters on exactly this (see its Programs2.cs::GetUWPApps()) to drop
// vendor/OEM-signed utilities (GPU control panels, audio consoles) that
// register as packaged apps but were never distributed through the Store,
// and would otherwise flood this list alongside actual Store/Game Pass
// titles. shell:AppsFolder's IShellItem properties have no equivalent.
//
// This still can't tell a game apart from any other Store app — Calculator
// and Paint are Store-signed too. Nothing local can make that call; Playnite
// only gets it by cross-referencing each package against Microsoft's Xbox
// Live catalog over an authenticated account, which is out of scope here.
// That filtering is left to the user picking from the list, same as
// everywhere else in this file.
std::vector<InstalledGame> EnumeratePackagedApps() {
    std::vector<InstalledGame> games;

    try {
        // Matches the STA the classic-COM shortcut resolution above already
        // set up via CoInitializeEx — WinRT's activation layer needs its own
        // compatible init call on top of that, not a competing one.
        winrt::init_apartment(winrt::apartment_type::single_threaded);

        winrt::Windows::Management::Deployment::PackageManager manager;
        for (const auto& package : manager.FindPackagesForUser(L"")) {
            if (package.IsFramework() || package.IsResourcePackage()) continue;
            if (package.SignatureKind() != winrt::Windows::ApplicationModel::PackageSignatureKind::Store)
                continue;

            for (const auto& entry : package.GetAppListEntries()) {
                const std::wstring name(entry.DisplayInfo().DisplayName());
                const std::wstring aumid(entry.AppUserModelId());
                if (name.empty() || aumid.empty()) continue;
                games.push_back({ name, L"aumid:" + aumid });
            }
        }
    } catch (const winrt::hresult_error&) {
        // Best-effort: an empty result here just means the picker falls back
        // to whatever the Start Menu walk found on its own.
    }

    return games;
}

}  // namespace

namespace GameLibrary {

std::vector<InstalledGame> EnumerateInstalled() {
    std::vector<InstalledGame> games;

    // COM may already be initialized on this thread (WebView2 runs on the
    // same UI thread once opened) — only tear down what we set up ourselves.
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOwnedByUs = coInit == S_OK || coInit == S_FALSE;

    std::vector<std::wstring> shortcutPaths;
    for (REFKNOWNFOLDERID folder : { FOLDERID_CommonPrograms, FOLDERID_Programs }) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(folder, 0, nullptr, &path)) && path)
            WalkDirectory(path, shortcutPaths);
        if (path) CoTaskMemFree(path);
    }

    std::unordered_set<std::wstring> seen;
    for (const auto& shortcutPath : shortcutPaths) {
        std::wstring id;

        bool isSteamLink = false;
        if (HasExtension(shortcutPath, L".url")) {
            id = ResolveUrlSteamId(shortcutPath);
            if (id.empty()) continue;  // not a "play this game" link
            isSteamLink = true;
        } else {
            Microsoft::WRL::ComPtr<IShellLinkW> link;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&link))))
                continue;
            Microsoft::WRL::ComPtr<IPersistFile> file;
            if (FAILED(link.As(&file)) || FAILED(file->Load(shortcutPath.c_str(), STGM_READ)))
                continue;

            id = ResolveLnkTarget(link.Get());
            if (id.empty()) continue;  // packaged app — not this pass
            if (!HasExtension(id, L".exe")) continue;
            if (GetFileAttributesW(id.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;  // shortcut is stale — target no longer exists
            if (IsUnderWindowsDir(id)) continue;
        }

        // The shortcut's own filename is the friendly name a user or
        // installer chose ("Rainbow Six Siege"); the exe's own name rarely
        // is ("RainbowSix.exe") — so the display name comes from the link,
        // not the target.
        std::wstring name = NameFromShortcutPath(shortcutPath);
        // A steam://rungameid/ link is already proof this is a real,
        // launchable game — Steam's own non-game links (support center, EULA)
        // don't take that form, so the noise-word filter only has false
        // positives left to contribute here (a game title that happens to
        // contain one of those words, as this app's own test machine has).
        if (!isSteamLink && LooksLikeNoise(name)) continue;

        if (!seen.insert(DedupKey(id)).second)
            continue;  // already have this game from another shortcut

        games.push_back({ std::move(name), std::move(id) });
    }

    for (auto& game : EnumeratePackagedApps()) {
        if (!seen.insert(DedupKey(game.id)).second)
            continue;
        games.push_back(std::move(game));
    }

    if (comOwnedByUs) CoUninitialize();

    std::sort(games.begin(), games.end(), [](const InstalledGame& a, const InstalledGame& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return games;
}

}  // namespace GameLibrary
