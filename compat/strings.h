/* compat/strings.h — POSIX string functions for MSVC */
#ifndef WISP_COMPAT_STRINGS_H
#define WISP_COMPAT_STRINGS_H

#include <string.h>

#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

#endif