// language: C, file: wisp-code/port.h
#ifndef WISP_PORT_H
#define WISP_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------- */
/*  Process ID                                                     */
/* -------------------------------------------------------------- */

#ifdef _WIN32
  #include <process.h>
  #define wisp_getpid() ((long)_getpid())
#else
  #include <unistd.h>
  #define wisp_getpid() ((long)getpid())
#endif

/* -------------------------------------------------------------- */
/*  Shell path                                                     */
/*                                                              */
/*  Returns the platform's default POSIX-ish shell. On Windows    */
/*  returns "cmd.exe" — the caller uses this for sh -c style      */
/*  invocation, and cmd /c matches the calling convention.        */
/* -------------------------------------------------------------- */

const char *wisp_shell_path(void);

/* -------------------------------------------------------------- */
/*  Directory walk                                                 */
/*                                                              */
/*  Callback receives a relative path (relative to the walk's    */
/*  root) and a flag: 1 for directory, 0 for regular file. The   */
/*  callback returns 0 to continue, non-zero to stop the walk.   */
/* -------------------------------------------------------------- */

typedef int (*wisp_walk_cb)(const char *rel, int is_dir, void *user);

int wisp_walk_dir(const char *root, wisp_walk_cb cb, void *user);

/* -------------------------------------------------------------- */
/*  Subprocess capture                                             */
/*                                                              */
/*  Runs `cmd` via the platform shell, captures combined          */
/*  stdout+stderr, and returns a malloc'd, NUL-terminated buffer. */
/*  `*exit_code` receives the process exit status (or -1).       */
/*  Returns NULL on spawn failure.                                */
/* -------------------------------------------------------------- */

char *wisp_run_capture(const char *cmd, int *exit_code);

/* -------------------------------------------------------------- */
/*  Path canonicalisation                                          */
/*                                                              */
/*  Resolves `in` to an absolute, symlink-free path where the    */
/*  platform supports it. Writes into `out`. If resolution       */
/*  fails, copies `in` verbatim. Never overflows.                 */
/* -------------------------------------------------------------- */

void wisp_canonicalize_root(const char *in, char *out, size_t out_sz);

/* -------------------------------------------------------------- */
/*  Monotonic clock                                                */
/* -------------------------------------------------------------- */

long long wisp_monotonic_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* WISP_PORT_H */