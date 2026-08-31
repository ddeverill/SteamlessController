#pragma once

#define IDI_ICON_OFF    101
#define IDI_ICON_ON     102
// Game mode is live, but on a handle shared with another writer (typically
// Steam) rather than an exclusive one — see ControllerManager::IsGameModeShared.
#define IDI_ICON_SHARED 103

// One definition of the version, for the VERSIONINFO resource in app.rc and for
// the startup line in the event log. Both matter: a build with no version stamp
// is indistinguishable from any other once it is installed, and a log that does
// not say which build wrote it costs a round trip with whoever reported the bug.
//
// Keep in step with MyAppVersion in resources/InnoInstallerScript.iss.
#define APP_VERSION_MAJOR 1
#define APP_VERSION_MINOR 19
#define APP_VERSION_PATCH 0
#define APP_VERSION_STR   "1.19"
