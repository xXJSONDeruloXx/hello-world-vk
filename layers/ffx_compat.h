#pragma once
// Compatibility shims for building a subset of FidelityFX SDK on non-MSVC compilers (Linux/GCC/Clang)
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <locale>
#include <codecvt>
#include <cmath>

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

// Map Windows secure string functions to standard ones (inputs assumed sane in SDK usage)
#ifdef wcscpy_s
#undef wcscpy_s
#endif
inline int ffx_wcscpy_s(wchar_t* dest, size_t /*destsz*/, const wchar_t* src){ std::wcscpy(dest, src); return 0; }
inline int ffx_wcscpy_s(wchar_t* dest, const wchar_t* src){ std::wcscpy(dest, src); return 0; }
#define wcscpy_s(...) ffx_wcscpy_s(__VA_ARGS__)

#ifdef strcpy_s
#undef strcpy_s
#endif
inline int ffx_strcpy_s(char* dest, size_t /*destsz*/, const char* src){ std::strcpy(dest, src); return 0; }
inline int ffx_strcpy_s(char* dest, const char* src){ std::strcpy(dest, src); return 0; }
#define strcpy_s(...) ffx_strcpy_s(__VA_ARGS__)
#ifndef swprintf_s
#define swprintf_s swprintf
#endif
#ifndef sprintf_s
#define sprintf_s snprintf
#endif

// Bring common math functions into global scope without macros (macros broke <chrono> templates)
using std::log2;
using std::floor;

#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif

// If we want to avoid the verbose breadcrumbs printing (uses sprintf_s heavily), we could
// define a flag; for now we emulate sprintf_s above so nothing else required.
