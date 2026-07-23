#ifndef CETRA_COMPAT_H
#define CETRA_COMPAT_H

// Small libc portability shim. strcasecmp / strncasecmp are POSIX, declared in
// <strings.h>; MSVC has neither the header nor the names, only _stricmp /
// _strnicmp. Include this instead of <strings.h>.

#if defined(_WIN32)
#include <string.h>
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

#endif // CETRA_COMPAT_H
