// Force-included into host code: names TotK Recomp's MinGW build gets from its C library that the MSVC-targeted
// LLVM Clang build has under other names.
#pragma once
#if defined(_MSC_VER)
#include <string.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif
