#define NOMINMAX  // avoid Windows.h's min/max macros colliding with C++/WinRT templates
#include "GameLibrary.h"
#include "LauncherGames.h"
#include "ProcessIdentity.h"
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

// The executables a launcher's own shortcuts point at. Every game a launcher
// installs gets a .lnk targeting one of these with a per-game argument, and
// IShellLink::GetPath hands back only the executable — so without this the
// whole Blizzard library resolves to one "Battle.net Launcher.exe" entry and
// DedupKey collapses it to a single wrongly-named game. Skipping them costs
// nothing now that LauncherGames reads each launcher's own records, which
// name the games properly and give a directory to match on.
//
// EA is the exception: its catalogue is encrypted with a key derived from the
// machine's hardware, so there is no enumerator for it and its games are
// simply absent rather than present-but-wrong. Absent is the better failure,
// and the manual picker in the remap window covers it.
bool IsLauncherStub(const std::wstring& exePath) {
    static const wchar_t* kStubs[] = {
        L"battle.net launcher.exe", L"battle.net.exe",
        L"eadesktop.exe", L"eabackgroundservice.exe", L"origin.exe",
        L"upc.exe", L"ubisoftconnect.exe", L"uplay.exe",
        L"epicgameslauncher.exe", L"galaxyclient.exe",
    };

    const size_t slash = exePath.find_last_of(L'\\');
    std::wstring leaf = slash == std::wstring::npos ? exePath
                                                    : exePath.substr(slash + 1);
    for (wchar_t& c : leaf) c = static_cast<wchar_t>(towlower(c));
    for (const wchar_t* stub : kStubs)
        if (leaf == stub) return true;
    return false;
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
// "steam://" URI, an "aumid:" id or a "dir:" one risks mangling them for no
// reason, since none of the three follows filesystem path rules — the "dir:"
// prefix in particular would be read as a relative first segment.
std::wstring DedupKey(const std::wstring& id) {
    std::wstring key;
    if (id.rfind(L"steam://", 0) == 0 || id.rfind(L"aumid:", 0) == 0
        || id.rfind(L"dir:", 0) == 0) {
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
                games.push_back({ name, L"aumid:" + aumid, GameSource::Packaged });
            }
        }
    } catch (const winrt::hresult_error&) {
        // Best-effort: an empty result here just means the picker falls back
        // to whatever the Start Menu walk found on its own.
    }

    return games;
}

// Lowercased, with a trailing separator, so a prefix test cannot match a
// sibling folder whose name merely starts the same way ("Portal" against
// "Portal 2") — the same convention SteamAppLocator uses on its own map.
std::wstring ContainmentPrefix(const std::wstring& dir) {
    std::wstring key = dir;
    for (wchar_t& c : key) c = static_cast<wchar_t>(towlower(c));
    if (!key.empty() && key.back() != L'\\') key += L'\\';
    return key;
}

bool IsInsideAny(const std::wstring& exePath, const std::vector<std::wstring>& prefixes) {
    std::wstring lower = exePath;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
    for (const auto& prefix : prefixes)
        if (lower.compare(0, prefix.size(), prefix) == 0) return true;
    return false;
}

}  // namespace

struct LangCodePage { WORD language; WORD codePage; };

// One string out of a loaded version resource, for a given language block.
std::wstring VersionString(const std::vector<BYTE>& block, LangCodePage lang,
                           const wchar_t* field) {
    wchar_t query[80];
    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%ls",
               lang.language, lang.codePage, field);

    wchar_t* value = nullptr;
    UINT     chars = 0;
    if (!VerQueryValueW(const_cast<BYTE*>(block.data()), query,
                        reinterpret_cast<LPVOID*>(&value), &chars)
        || !value)
        return {};

    std::wstring text(value, chars);
    // The returned length includes the terminator, and some resources pad.
    while (!text.empty() && (text.back() == L'\0' || text.back() == L' '))
        text.pop_back();
    return text;
}

// The name an executable calls itself in its version resource — "Among Us"
// for AmongUs.exe, "Firefox" for firefox.exe. Far better prose than a
// filename, and the only name a portable game with no installer has anywhere
// at all.
static std::wstring FileDescriptionOf(const std::wstring& exePath) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(exePath.c_str(), &ignored);
    if (size == 0) return {};

    std::vector<BYTE> block(size);
    if (!GetFileVersionInfoW(exePath.c_str(), 0, size, block.data())) return {};

    // An executable is supposed to declare which languages its strings are
    // in, and the string blocks are keyed by that. Plenty do not — several
    // Microsoft Store binaries among them — so the two conventional keys are
    // tried as well: US English and language-neutral, both with the Unicode
    // codepage. Without that fallback those binaries yield nothing and the
    // caller ends up naming a profile after a window title.
    std::vector<LangCodePage> langs;
    LangCodePage* declared  = nullptr;
    UINT          langBytes = 0;
    if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation",
                       reinterpret_cast<LPVOID*>(&declared), &langBytes)
        && declared)
        langs.assign(declared, declared + langBytes / sizeof(LangCodePage));
    langs.push_back({ 0x0409, 0x04b0 });
    langs.push_back({ 0x0000, 0x04b0 });

    for (const LangCodePage& lang : langs) {
        // FileDescription is the friendlier of the two and what most binaries
        // fill in properly. ProductName is the better answer when a build has
        // put its own filename in FileDescription, which is exactly what
        // Windows' own Notepad does ("Notepad.exe" against "Notepad").
        const std::wstring description = VersionString(block, lang, L"FileDescription");
        const std::wstring product     = VersionString(block, lang, L"ProductName");

        const bool descriptionIsFilename =
            description.size() > 4
            && _wcsicmp(description.c_str() + description.size() - 4, L".exe") == 0;
        if (!description.empty() && !descriptionIsFilename) return description;
        if (!product.empty()) return product;
        if (!description.empty()) return description;
    }
    return {};
}

namespace {

// Background helpers that happen to own a visible window. A launcher itself
// is worth offering — someone may genuinely want a profile that applies while
// they are browsing their library — but these are pieces of one, doing a job
// on its behalf, and nobody is playing them. Steam is the offender in
// practice: its library and store are a Chromium instance, so
// steamwebhelper.exe owns a real titled window and passes every other test
// here.
bool IsHelperProcess(const std::wstring& exePath) {
    static const wchar_t* kHelpers[] = {
        L"steamwebhelper.exe",     // the CEF host behind Steam's own UI
        L"gameoverlayui.exe",      // the in-game overlay, drawn over a game
        L"steamerrorreporter.exe",
        L"steamerrorreporter64.exe",
        L"streaming_client.exe",   // Remote Play
    };

    const size_t slash = exePath.find_last_of(L'\\');
    std::wstring leaf = slash == std::wstring::npos ? exePath
                                                    : exePath.substr(slash + 1);
    for (wchar_t& c : leaf) c = static_cast<wchar_t>(towlower(c));
    for (const wchar_t* helper : kHelpers)
        if (leaf == helper) return true;
    return false;
}

BOOL CALLBACK CollectRunningWindow(HWND hwnd, LPARAM param) {
    auto* found = reinterpret_cast<std::vector<InstalledGame>*>(param);

    // What the user could alt-tab to, which is the same population they are
    // choosing from in their head. A window with no title, or one owned by
    // another window, is a tooltip or a palette rather than an application.
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;

    wchar_t title[256];
    const int len = GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    if (len <= 0) return TRUE;

    // ProcessIdentity resolves a packaged app past ApplicationFrameHost, so
    // Store and Xbox app titles name themselves here rather than all looking
    // like the same host process.
    const ForegroundIdentity id = ProcessIdentity::ForWindow(hwnd);
    if (id.exePath.empty()) return TRUE;  // protected, or closed under us

    // Windows itself keeps several windowed processes alive at all times, and
    // they are "running" by every test above: the desktop and taskbar
    // (explorer.exe), the frame packaged apps are hosted in
    // (ApplicationFrameHost.exe, when its real child could not be reached),
    // and the touch keyboard (TextInputHost.exe). The same rule that keeps
    // them out of the installed list keeps them out of this one.
    if (GameLibrary::IsSystemProgram(id.exePath)) return TRUE;
    if (IsHelperProcess(id.exePath)) return TRUE;

    // Our own window is in this list too, and a profile for it would be a
    // joke at the user's expense.
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    for (const auto& already : *found)
        if (_wcsicmp(already.id.c_str(), id.exePath.c_str()) == 0)
            return TRUE;  // one entry per application, not per window

    InstalledGame app;
    app.id     = id.exePath;
    app.source = GameSource::Manual;
    // The executable's own description first: a window title changes with
    // what the application is doing ("Untitled - Notepad", a level name), and
    // would be a poor thing to label a saved profile with. The title is still
    // the better fallback, since a binary with no version resource at all is
    // usually the sort of small game that has no installer either.
    app.name = FileDescriptionOf(id.exePath);
    if (app.name.empty()) app.name.assign(title, static_cast<size_t>(len));
    if (app.name.empty()) app.name = GameLibrary::NameForExecutable(id.exePath);

    found->push_back(std::move(app));
    return TRUE;
}

}  // namespace

namespace GameLibrary {

std::wstring NameForExecutable(const std::wstring& exePath) {
    std::wstring name = FileDescriptionOf(exePath);
    // NameFromShortcutPath does exactly the right thing to a full path too:
    // take the leaf, drop the extension.
    if (name.empty()) name = NameFromShortcutPath(exePath);
    return name;
}

std::vector<InstalledGame> EnumerateRunning() {
    std::vector<InstalledGame> found;
    EnumWindows(CollectRunningWindow, reinterpret_cast<LPARAM>(&found));
    std::sort(found.begin(), found.end(), [](const InstalledGame& a, const InstalledGame& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return found;
}

bool IsSystemProgram(const std::wstring& exePath) {
    wchar_t winDir[MAX_PATH];
    if (!GetWindowsDirectoryW(winDir, MAX_PATH)) return false;
    const size_t len = wcslen(winDir);
    return exePath.size() > len
        && _wcsnicmp(exePath.c_str(), winDir, len) == 0
        && (exePath[len] == L'\\' || exePath[len] == L'\0');
}

}  // namespace GameLibrary

const wchar_t* GameSourceName(GameSource source) {
    switch (source) {
        case GameSource::StartMenu: return L"start-menu";
        case GameSource::Steam:     return L"steam";
        case GameSource::Packaged:  return L"packaged";
        case GameSource::Epic:      return L"epic";
        case GameSource::Gog:       return L"gog";
        case GameSource::Ubisoft:   return L"ubisoft";
        case GameSource::BattleNet: return L"battle.net";
        case GameSource::Manual:    return L"manual";
        case GameSource::Missing:   return L"missing";
    }
    return L"unknown";
}

namespace GameLibrary {

std::vector<InstalledGame> EnumerateInstalled() {
    std::vector<InstalledGame> games;

    // COM may already be initialized on this thread (WebView2 runs on the
    // same UI thread once opened) — only tear down what we set up ourselves.
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOwnedByUs = coInit == S_OK || coInit == S_FALSE;

    // First, because the shortcut walk below is filtered against what this
    // finds: a launcher's own records name a game better than its shortcut
    // does, and give an install directory rather than one executable.
    std::unordered_set<std::wstring> seen;
    std::vector<std::wstring>        launcherDirs;
    for (auto& game : LauncherGames::EnumerateAll()) {
        if (!seen.insert(DedupKey(game.id)).second) continue;
        launcherDirs.push_back(ContainmentPrefix(game.id.substr(4)));  // past "dir:"
        games.push_back(std::move(game));
    }

    std::vector<std::wstring> shortcutPaths;
    // Both Desktop folders as well as both Start Menu ones: plenty of
    // installers — Epic's "create desktop shortcut" among them — offer only a
    // desktop icon, and a game with no Start Menu entry was invisible here.
    for (REFKNOWNFOLDERID folder : { FOLDERID_CommonPrograms, FOLDERID_Programs,
                                     FOLDERID_PublicDesktop, FOLDERID_Desktop }) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(folder, 0, nullptr, &path)) && path)
            WalkDirectory(path, shortcutPaths);
        if (path) CoTaskMemFree(path);
    }

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
            if (IsSystemProgram(id)) continue;
            if (IsLauncherStub(id)) continue;
            // The launcher pass already has this game, under a directory id
            // that matches more of it than this one executable would.
            if (IsInsideAny(id, launcherDirs)) continue;
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

        games.push_back({ std::move(name), std::move(id),
                          isSteamLink ? GameSource::Steam : GameSource::StartMenu });
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
