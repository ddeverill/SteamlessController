#pragma once
#include "core/AppSettings.h"
#include "core/ControllerManager.h"
#include "core/SettingsCodec.h"
#include "core/iface/IPlatform.h"
#include "core/iface/IScheduler.h"
#include "core/iface/ISteamWatcher.h"
#include "platform/linux/LinuxPlatform.h"
#include "IpcServer.h"
#include <atomic>
#include <string>
#include <vector>

// Owns ControllerManager and the control socket, and holds the policy that
// in TrayApp lives alongside the tray shell: auto-mode gating off
// ISteamWatcher, which profile is active, and persisting settings changes
// back through ISettingsStore. A full ControllerService extraction (shared
// verbatim between this and a future TrayApp rewrite) is future work — see
// the port's plan — so this policy is duplicated here rather than shared
// for now; it is a small, self-contained amount of logic.
class Daemon {
public:
    explicit Daemon(LinuxPlatform& platform);

    // Blocks, running the daemon's single-threaded event loop, until
    // RequestStop() is called (typically from a signal handler). Every
    // ControllerManager-touching callback — IPC commands, scheduler timers,
    // Steam-state/device-change notifications — runs from inside this one
    // loop; background threads (LinuxSteamWatcher, LinuxDeviceMonitor) only
    // ever reach it by posting through IScheduler, never by calling back
    // directly. See LinuxScheduler's class comment for why that split
    // matters.
    void Run();
    void RequestStop() { m_running = false; }

private:
    void OnControllerStateChanged(bool connected, bool gameModeActive, bool padUnavailable);
    void OnAlert(const std::string& title, const std::string& text);
    void OnSteamStateChanged(SteamState state);

    void LoadSettingsFromStore();
    void PersistSettings();  // debounced via IScheduler::After

    // Applies m_activeProfile (the default profile merged with a per-game
    // override, if any) to m_controller and, if game mode is already
    // active, live.
    void ApplyActiveProfile();
    const ControllerProfile& EffectiveProfile() const;
    SettingsCodec::GameProfileEntry* FindGameProfile(const std::string& id);

    CommandResult HandleCommand(const JsonValue& req);
    JsonValue BuildStatus() const;
    JsonValue BuildDevices() const;
    JsonValue BuildDoctor() const;

    LinuxPlatform&     m_platform;

    // Declared, and therefore constructed, before m_controller: building
    // ControllerManager runs SyncDevices() synchronously, which can invoke
    // our state-changed callback (OnControllerStateChanged) before
    // ControllerManager's constructor even returns. That callback touches
    // m_ipc and EffectiveProfile()'s profile members — if any of them were
    // declared after m_controller, this call would land on a not-yet-
    // constructed object (undefined behavior; this is what the segfault
    // during startup turned out to be before this ordering was fixed).
    // Member construction order follows declaration order regardless of
    // the constructor's initializer-list order, so this ordering is the
    // actual fix, not just documentation of intent.
    IpcServer          m_ipc;
    AppSettings        m_settings;
    ControllerProfile  m_defaultProfile;
    std::vector<SettingsCodec::GameProfileEntry> m_gameProfiles;
    std::string        m_activeGameId;  // "" = default profile is active

    ControllerManager  m_controller;

    bool               m_wantControl = false;  // the `control acquire`/`release` toggle
    SteamState         m_lastSteamState = SteamState::NoSteam;

    bool               m_settingsDirty = false;
    IScheduler::TimerId m_persistTimer = 0;

    std::atomic<bool>  m_running{true};
};
