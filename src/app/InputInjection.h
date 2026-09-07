#pragma once
#include <Windows.h>
#include <functional>
#include <string>

// Synthetic input, with the reasons it can fail recorded.
//
// Every key, mouse button and cursor move this app produces leaves through
// SendInput, and SendInput is the one stage of the pipeline Windows can refuse
// without any other symptom: haptics and the virtual pad are HID writes and
// keep working regardless. A refused injection therefore looks exactly like
// "the trackpad stopped moving the mouse, but the pad still buzzes", with
// nothing in the log to say why.
//
// The refusal that matters is UIPI: a process may not inject input while the
// foreground window belongs to a process at a higher integrity level, so an
// elevated game, launcher or tool in the foreground silently swallows
// everything this app sends until focus moves elsewhere or that window closes.
// This app never runs elevated (see SteamlessDeviceCycle) precisely so that it
// is not the one doing the blocking, which leaves it on the receiving end.
//
// The refusal is not visible in SendInput's result: on Windows 11 the call
// reports success and the event is discarded afterwards, so the only reliable
// way to know is to ask, before injecting, whether the foreground window
// outranks this process. Send does that on every event and reports the state
// changing — which covers cursor movement, trackpad clicks, paddle buttons and
// keys alike, none of which can be checked from their own return value.
//
// Injection can also be accepted and still do nothing for reasons that are not
// UIPI — a game that left the cursor clipped, or a low-level hook dropping
// injected events — so movement is checked against the cursor actually moving
// as well.
namespace InputInjection {

// How the app tells the user something is wrong (the tray balloon). Called from
// a controller read thread, so the callback must marshal to the UI thread — the
// tray's existing alert path already does.
using AlertFn = std::function<void(const std::wstring& title,
                                   const std::wstring& text)>;
void SetAlertCallback(AlertFn fn);

// SendInput for a single event. 'what' is a short tag naming the caller
// ("trackpad-move", "trackpad-click", "paddle-mouse", "key") so a log line can
// say what was lost. Returns whether Windows accepted the event — which is not
// the same as the event arriving; see above.
bool Send(const INPUT& input, const char* what);

// Lines per wheel notch that the injected scroll scale is calibrated against,
// which is the Windows default. A machine set to anything else has every wheel
// event we send multiplied by a different number on arrival, so identical
// injection scrolls a different distance there; dividing the scale by
// WheelLinesPerNotch() below is what makes one swipe of a trackpad cover the
// same ground everywhere. See TrackpadInput::UpdateScroll.
//
// Anchoring at the default rather than at 1 is deliberate: the correction is
// then exactly 1.0 on a machine nobody has touched, so normalising changes
// nothing for the great majority of installs and only moves the ones that were
// already out of step.
inline constexpr float kCalibratedWheelLines = 3.0f;

// The factor the scroll scale is multiplied by on a machine reporting `lines`
// lines per notch. Pure and separated from the setting it is normally fed so
// the property that matters can be checked rather than argued about: it is
// exactly 1.0 at the default, which is what makes normalising safe to turn on
// for every install at once.
//
// Zero guards a divide against a system that reports nothing usable. It does
// not happen today, and a scroll that silently becomes infinite is not the way
// to discover that it started.
inline constexpr float WheelCorrection(float lines) {
    return lines > 0.0f ? kCalibratedWheelLines / lines : 1.0f;
}

// The system's "roll the mouse wheel to scroll N lines" setting, resolved to a
// number that scale can divide by. WHEEL_PAGESCROLL becomes an estimated
// page's worth of lines, and a setting that cannot be read at all becomes
// kCalibratedWheelLines, which corrects by exactly 1.0 and so leaves the scale
// where it was. Cached internally; cheap enough to call once per report.
float WheelLinesPerNotch();

// Everything that decides whether injection can land: the foreground window's
// process and integrity level, ours, any cursor clip, and whether the cursor
// can be read at all. Logged when game mode is taken, and whenever injection
// is refused. 'reason' leads the line.
void LogEnvironment(const char* reason);

// Injection was accepted but the cursor did not move. 'travelPx' is how much
// movement was sent while the cursor sat at 'at'. Rate-limited like refusals.
void LogCursorNotMoving(long travelPx, POINT at);

}  // namespace InputInjection
