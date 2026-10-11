#ifndef POSIX_H
#define POSIX_H

#if defined(_WIN32)

/* ------------------------------------------------------------------
 *  Windows compatibility layer.
 *
 *  The rest of the codebase is written against POSIX. This header
 *  provides just enough to compile on MSVC: the type ssize_t, the
 *  unistd.h accessors, strtok_r, localtime_r, usleep, S_ISREG, and
 *  the signal constants MSVC doesn't define.
 *
 *  On POSIX builds this header is a no-op.
 * ------------------------------------------------------------------ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <io.h>
#include <process.h>
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <windows.h>

/* ---- types ---- */
#ifndef _SSIZE_T_DEFINED
  typedef long long ssize_t;
  #define _SSIZE_T_DEFINED
#endif

/* ---- file descriptor constants ---- */
#ifndef STDOUT_FILENO
  #define STDOUT_FILENO 1
#endif
#ifndef STDIN_FILENO
  #define STDIN_FILENO 0
#endif
#ifndef STDERR_FILENO
  #define STDERR_FILENO 2
#endif

/* ---- POSIX names for MSVC CRT functions ---- */
#ifndef read
  #define read  _read
#endif
#ifndef write
  #define write _write
#endif
#ifndef close
  #define close _close
#endif
#ifndef unlink
  #define unlink _unlink
#endif
#ifndef getcwd
  #define getcwd _getcwd
#endif
#ifndef mkdir
  #define mkdir(p, m) _mkdir(p)
#endif
#ifndef rmdir
  #define rmdir _rmdir
#endif
#ifndef strdup
  #define strdup _strdup
#endif
#ifndef strcasecmp
  #define strcasecmp _stricmp
#endif
#ifndef strncasecmp
  #define strncasecmp _strnicmp
#endif
#ifndef fileno
  #define fileno _fileno
#endif

/* ---- rename (atomic replace on Windows) ---- */
static inline int wisp_rename(const char *oldpath, const char *newpath) {
    if (MoveFileExA(oldpath, newpath,
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return 0;
    }
    /* Translate the Win32 error into an errno-ish value so callers
     * that inspect errno still get something useful. */
    DWORD e = GetLastError();
    switch (e) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
        case ERROR_ACCESS_DENIED:  errno = EACCES; break;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:    errno = EEXIST; break;
        default:                   errno = EINVAL; break;
    }
    return -1;
}

#undef rename
#define rename wisp_rename

/* ---- strtok_r ---- */
static inline char *wisp_strtok_r(char *s, const char *delim, char **save) {
    return strtok_s(s, delim, save);
}
#define strtok_r wisp_strtok_r

/* ---- localtime_r ---- */
static inline struct tm *wisp_localtime_r(const time_t *t, struct tm *out) {
    if (localtime_s(out, t) != 0) return NULL;
    return out;
}
#define localtime_r wisp_localtime_r

/* ---- usleep ---- */
static inline int wisp_usleep(unsigned int usec) {
    Sleep(usec / 1000);
    return 0;
}
#define usleep wisp_usleep

/* ---- clock_gettime ---- */
#ifndef CLOCK_MONOTONIC
  #define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_REALTIME
  #define CLOCK_REALTIME 0
#endif

struct wisp_timespec { long tv_sec; long tv_nsec; };

static inline int wisp_clock_gettime(int clk, struct timespec *ts) {
    if (clk == CLOCK_MONOTONIC) {
        static LARGE_INTEGER s_freq = {0};
        LARGE_INTEGER now;
        if (s_freq.QuadPart == 0) QueryPerformanceFrequency(&s_freq);
        QueryPerformanceCounter(&now);
        ts->tv_sec  = (time_t)(now.QuadPart / s_freq.QuadPart);
        ts->tv_nsec = (long)(((now.QuadPart % s_freq.QuadPart) * 1000000000LL)
                             / s_freq.QuadPart);
        return 0;
    }
    /* CLOCK_REALTIME */
    {
        FILETIME ft;
        ULARGE_INTEGER u;
        GetSystemTimeAsFileTime(&ft);
        u.LowPart  = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        /* 100-ns intervals since 1601-01-01. Convert to Unix epoch. */
        unsigned long long ns100 = u.QuadPart - 116444736000000000ULL;
        ts->tv_sec  = (time_t)(ns100 / 10000000ULL);
        ts->tv_nsec = (long)((ns100 % 10000000ULL) * 100);
        return 0;
    }
}
#define clock_gettime(clk, ts) wisp_clock_gettime((clk), (ts))

/* ---- S_ISREG / S_ISDIR ---- */
#ifndef S_ISREG
  #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISDIR
  #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

/* ---- signals MSVC doesn't define ---- */
#ifndef SIGHUP
  #define SIGHUP  1
#endif
#ifndef SIGQUIT
  #define SIGQUIT 3
#endif

/* ---- CTRL_WINDOW_BUFFER_SIZE_EVENT ---- */
#ifndef CTRL_WINDOW_BUFFER_SIZE_EVENT
  #define CTRL_WINDOW_BUFFER_SIZE_EVENT 0x0004
#endif

/* ---- console input mode flags ---- */
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
  #define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
  #define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef DISABLE_NEWLINE_AUTO_RETURN
  #define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif

#else

  #include <unistd.h>
#endif /* _WIN32 */


#endif /* WISP_POSIX_H */