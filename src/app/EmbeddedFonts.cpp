#include "EmbeddedFonts.h"
#include "resource.h"
#include <iterator>

namespace {

const EmbeddedFont kFonts[] = {
    { L"Barlow-Regular.ttf",        IDR_FONT_BARLOW_REGULAR  },
    { L"Barlow-Medium.ttf",         IDR_FONT_BARLOW_MEDIUM   },
    { L"Barlow-SemiBold.ttf",       IDR_FONT_BARLOW_SEMIBOLD },
    { L"Barlow-Bold.ttf",           IDR_FONT_BARLOW_BOLD     },
    { L"JetBrainsMono-SemiBold.ttf", IDR_FONT_JBMONO_SEMIBOLD },
};

}  // namespace

size_t EmbeddedFontCount() { return std::size(kFonts); }

const EmbeddedFont& EmbeddedFontAt(size_t index) { return kFonts[index]; }

const EmbeddedFont* FindEmbeddedFont(std::wstring_view file) {
    for (const auto& font : kFonts)
        if (file == font.file) return &font;
    return nullptr;
}

bool LoadEmbeddedFont(HINSTANCE module, const EmbeddedFont& font, const void*& data, DWORD& size) {
    // RT_RCDATA by number: the macro is the ANSI form in a build without UNICODE,
    // which FindResourceW will not take.
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(font.resourceId), MAKEINTRESOURCEW(10));
    if (!res) return false;
    HGLOBAL loaded = LoadResource(module, res);
    if (!loaded) return false;
    data = LockResource(loaded);
    size = SizeofResource(module, res);
    return data != nullptr && size != 0;
}
