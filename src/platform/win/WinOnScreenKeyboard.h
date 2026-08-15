#pragma once

// The Windows touch keyboard, as something a paddle can be bound to (#71).
//
// Why this is a binding of its own rather than "bind a paddle to Ctrl+Win+O":
// that shortcut opens osk.exe, whose manifest carries uiAccess="true", so it
// runs at High integrity. UIPI forbids injecting input into a window whose
// process outranks yours, and this app is Medium by design (InputInjection.h),
// so every trackpad click aimed at osk.exe is accepted and then discarded —
// the keyboard appears and not one of its keys can be pressed. Running the app
// elevated does fix it, at the price of a UAC prompt on every launch and the
// cursor-freeze bug that keeps the tray unelevated in the first place.
//
// The touch keyboard is a different window, and the difference is the whole
// point. Measured on Windows 11:
//
//   TabTip.exe         High   (uiAccess=true)  broker; owns no visible window
//   TextInputHost.exe  Medium                  owns "Windows Input Experience",
//                                              which is what the user clicks
//
// TextInputHost sits at our own integrity level, so injected clicks land on it
// with nothing elevated anywhere. TabTip is only ever asked to show the
// keyboard; it is never the thing being clicked.
namespace TouchKeyboard {

// Shows the keyboard when it is hidden, hides it when it is shown.
//
// Returns immediately. The toggle is a cross-process call into TabTip and can
// block, and every caller is a controller read loop running at report rate, so
// the work is handed to a worker thread — see the .cpp.
void Toggle();

}  // namespace TouchKeyboard
