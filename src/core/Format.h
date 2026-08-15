#pragma once

// printf-style format-string checking, spelled for whichever compiler is
// building this translation unit. MSVC's SAL annotation and GCC/Clang's
// __attribute__((format)) both catch a specifier that doesn't match its
// argument at compile time — worth keeping on both, since these are
// diagnostic log lines read long after the fact, by whoever is trying to
// explain a fault they cannot reproduce.
#if defined(_MSC_VER)
  #include <sal.h>
  #define SC_PRINTF_FORMAT _Printf_format_string_
  #define SC_PRINTF_CHECK(fmtIndex, argIndex)
#else
  #define SC_PRINTF_FORMAT
  #define SC_PRINTF_CHECK(fmtIndex, argIndex) __attribute__((format(printf, fmtIndex, argIndex)))
#endif
