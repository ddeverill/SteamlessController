#pragma once
#include <Windows.h>
#include <cstddef>
#include <string_view>

// The fonts built into the executable (resources/fonts.rc): Barlow in four weights
// and JetBrains Mono SemiBold, both under the SIL Open Font License.
//
// Built in rather than installed beside the program so there is no file layout to
// get wrong — a copied-out exe, a dev build and the installed one all look the
// same — and so the fonts travel inside what gets signed. One copy of each file
// serves both consumers: DirectWrite reads it for the tray panel, and the
// Customize page is handed the same bytes over WebView2.
// Where the Customize page believes the fonts live. The host does not exist; the
// window answers requests for it from the embedded copies before they could go
// anywhere. The page's @font-face rules spell the same origin out by hand.
inline constexpr wchar_t kEmbeddedFontOrigin[] = L"https://steamless.fonts/";

struct EmbeddedFont {
    // The file name the page asks for, e.g. L"Barlow-Medium.ttf".
    const wchar_t* file;
    int            resourceId;
};

size_t              EmbeddedFontCount();
const EmbeddedFont& EmbeddedFontAt(size_t index);

// The font a page request names, or null for anything we do not carry.
const EmbeddedFont* FindEmbeddedFont(std::wstring_view file);

// Points into the module's resource section, so the data lives as long as the
// process and nothing needs freeing.
bool LoadEmbeddedFont(HINSTANCE module, const EmbeddedFont& font, const void*& data, DWORD& size);
