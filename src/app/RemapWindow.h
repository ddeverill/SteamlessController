#pragma once
#include <Windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "BackButtonConfig.h"
#include "ControllerPlatform.h"
#include "GameLibrary.h"
#include "TrackpadConfig.h"

class ControllerManager;

// One row the picker can offer, and the token the page names it by.
//
// The page never sees a raw game id — those hold non-ASCII and backslashes
// the hand-rolled JSON channel would mangle — so it addresses an entry by an
// opaque decimal token instead. A token is issued once and never reused or
// renumbered, which matters because the list changes underneath the page
// twice: when the background enumeration lands, and whenever the user adds an
// application by hand. A positional index would silently come to mean a
// different game at both of those moments.
struct PickerEntry {
    InstalledGame game;
    size_t        token = 0;
};

class RemapWindow {
public:
    RemapWindow() = default;
    ~RemapWindow();
    RemapWindow(const RemapWindow&) = delete;
    RemapWindow& operator=(const RemapWindow&) = delete;

    // Opens (or re-focuses) the remap window.
    // Shows a "not supported" message box and returns without opening if WebView2
    // is unavailable (Windows 7/8 or no runtime installed).
    // gameProfiles supplies whatever overrides already exist. The picker fills
    // itself: finding the installed games takes a second or more, so the
    // window goes up first and the list arrives afterwards.
    // applyCallback fires on the UI thread when the user clicks Apply — the
    // game id identifies whichever game was selected, or is empty for the
    // default profile.
    // deleteCallback fires when the user removes a game's profile; its id is
    // one that was in gameProfiles, and never empty — the default profile
    // cannot be removed.
    void Open(HINSTANCE hInst, ControllerManager* mgr, const ControllerProfile& profile,
              std::map<std::wstring, ControllerProfile> gameProfiles,
              std::function<void(const std::wstring&, const ControllerProfile&)> applyCallback,
              std::function<void(const std::wstring&)> deleteCallback);

    void BringToFront() const;
    // Open in the sense that matters to callers: visible and being edited.
    // A window that has been closed is only hidden, not destroyed, so the
    // handle alone does not answer this.
    bool IsOpen()       const { return m_hwnd != nullptr && IsWindowVisible(m_hwnd); }

    // Fires when the window is dismissed. Set once and kept for the object's
    // lifetime — profile switching is suppressed while the window is up, so
    // somebody has to re-evaluate once it comes down.
    void SetOnClose(std::function<void()> fn) { m_onClose = std::move(fn); }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // Add a picker entry for every saved profile no enumerated game accounts
    // for, so a profile for something the list cannot see is still reachable —
    // to look at, or to delete.
    //
    // Two quite different things end up here. One is a game that really has
    // gone: it gets GameSource::Missing and the picker says so. The other is
    // an application the user pointed at by hand, which was never going to be
    // in the installed list and is working perfectly; that one is checked for
    // on disk and marked GameSource::Manual, so a profile that is doing its
    // job is not labelled as broken.
    void AppendOrphanProfiles();

    // Runs GameLibrary::EnumerateInstalled() on a background thread and posts
    // the result back as WM_GAMES_READY. Started once per opening.
    void StartEnumeration();

    // Adds a picker entry the user chose themselves, selects it on the page,
    // and returns its token. Nothing is saved until they hit Apply.
    void AddManualGame(InstalledGame game);

    // The two ways to name an application the installed list does not have.
    void SendRunningApps();
    void BrowseForExe();

    // The entry the page means by this token, or null for an absent, malformed
    // or unknown one — a message we did not send, which no handler acts on.
    PickerEntry* EntryForToken(const std::string& value);

    // This game's token, issuing one the first time it is asked for.
    size_t TokenFor(const std::wstring& id);

    // Hands the page the current list, plus the profiles that go with it.
    // Distinct from SendInitState, which also resets which game is selected —
    // the list can land while the user is already editing, and throwing their
    // work away because the enumeration finished would be indefensible.
    void SendGameList();

    void CreateWebViewAsync(HWND hwnd);
    void OnControllerReady(ICoreWebView2Controller* ctrl);
    void OnWebMessage(const std::wstring& raw);
    void PostToWebView(const std::wstring& jsonStr);
    void SendInitState();

    static std::wstring ProfileJson(const ControllerProfile& p);
    std::wstring GamesJson() const;
    std::wstring ProfilesJson() const;

    // Tells the page a binding was captured. The page applies it to whichever
    // row is listening — it owns that state, not us.
    void PostCapturedBinding(const BackButtonBinding& binding);

    // Called from the read thread via PostMessage — marshals a captured button
    // back to the UI thread so we can call PostWebMessageAsString safely.
    static constexpr UINT WM_BUTTON_CAPTURED = WM_APP + 1;
    // The page has resolved its unsaved-changes prompt and the window may now
    // be hidden. Distinct from WM_CLOSE so the confirmed close does not loop
    // back into asking the page again.
    static constexpr UINT WM_CLOSE_CONFIRMED = WM_APP + 2;
    // The background enumeration finished. LPARAM owns a
    // std::vector<InstalledGame> the handler takes and deletes.
    static constexpr UINT WM_GAMES_READY = WM_APP + 3;

    HWND              m_hwnd     = nullptr;
    HINSTANCE         m_hInst   = nullptr;
    ControllerManager* m_mgr    = nullptr;
    ControllerProfile m_config;
    // Everything the picker can offer: the enumerated games, then one entry
    // per profile they did not account for, then anything the user added by
    // hand this session. Kept as one list so an entry of any kind is applied
    // and deleted through the paths that already existed.
    std::vector<PickerEntry>                   m_games;
    // Game id to the token the page knows it by. Kept for the window's whole
    // life and never renumbered, so rebuilding m_games — which happens every
    // time an enumeration lands — cannot change what a token the page is
    // already holding refers to.
    std::map<std::wstring, size_t>             m_tokens;
    size_t                                     m_nextToken = 1;
    // The applications offered by the "running now" picker, held between
    // sending the list and the user choosing from it. Same token discipline.
    std::vector<PickerEntry>                   m_runningApps;
    std::map<std::wstring, ControllerProfile>  m_gameProfiles;
    std::function<void(const std::wstring&, const ControllerProfile&)> m_applyCallback;
    std::function<void(const std::wstring&)> m_deleteCallback;
    std::function<void()> m_onClose;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_env;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller>  m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2>            m_webview;

    static RemapWindow* s_instance;

    static constexpr wchar_t CLASS_NAME[] = L"SteamlessRemapWindow";
    // Design sizes in logical (96-DPI) pixels — scaled to the monitor's DPI
    // at creation. The process is PER_MONITOR_AWARE_V2, so nothing scales
    // these for us.
    static constexpr int WINDOW_W = 760;
    static constexpr int WINDOW_H = 668; // 46 titlebar + ~560 body + 62 footer
    static constexpr int MIN_WINDOW_W = 560;
    static constexpr int MIN_WINDOW_H = 440;
};
