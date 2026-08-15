#pragma once
#include <string>

// UTF-8 <-> wide-string bridge for the Windows boundary. Everywhere in
// core/ and the platform-neutral parts of the app, strings are UTF-8
// std::string — this is the one place that crosses back to UTF-16 for the
// Win32 APIs that still require it (paths, registry values). Linux has no
// use for this header at all; nothing outside src/platform/win and src/win
// should need it.
#if defined(_WIN32)
std::wstring Utf8ToWide(const std::string& utf8);
std::string  WideToUtf8(const std::wstring& wide);
#endif
