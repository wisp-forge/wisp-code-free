// language: C, file: wisp-code/port.c
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <limits.h>        /* PATH_MAX */
#include "console.h"       /* MAX_PATH_LEN */

/* ================================================================== */
/*  Shell path                                                        */
/* ================================================================== */

const char *wisp_shell_path(void) {
#if defined(_WIN32)
    return "cmd.exe";
#elif defined(WISP_TERMUX) || defined(__ANDROID__)
    /* Termux has no /bin/sh; the shell lives under $PREFIX. */
    const char *prefix = getenv("PREFIX");
    static char buf[512];
    if (prefix && prefix[0]) {
        snprintf(buf, sizeof(buf), "%s/bin/sh", prefix);
        return buf;
    }
    return "/system/bin/sh";
#else
    return "/bin/sh";
#endif
}

/* ================================================================== */
/*  Monotonic clock                                                   */
/* ================================================================== */

long long wisp_monotonic_ms(void) {
#if defined(_WIN32)
    /* QueryPerformanceCounter is monotonic; convert to ms. */
    #include <windows.h>   /* pulled in here so port.c doesn't need it globally */
    static LARGE_INTEGER freq = {0};
    static int init = 0;
    if (!init) { QueryPerformanceFrequency(&freq); init = 1; }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (long long)((now.QuadPart * 1000LL) / freq.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000LL + t.tv_nsec / 1000000LL;
#endif
}

/* ================================================================== */
/*  Path canonicalisation                                             */
/* ================================================================== */

void wisp_canonicalize_root(const char *in, char *out, size_t out_sz) {
    if (!in || !in[0] || !out || out_sz < 2) {
        if (out && out_sz) out[0] = '\0';
        return;
    }

#if defined(_WIN32)
    char resolved[_MAX_PATH];
    if (_fullpath(resolved, in, sizeof(resolved))) {
        size_t n = strlen(resolved);
        if (n >= out_sz) n = out_sz - 1;
        memcpy(out, resolved, n);
        out[n] = '\0';
    } else {
        size_t n = strlen(in);
        if (n >= out_sz) n = out_sz - 1;
        memcpy(out, in, n);
        out[n] = '\0';
    }
#else
    char resolved[PATH_MAX];
    if (realpath(in, resolved)) {
        size_t n = strlen(resolved);
        if (n >= out_sz) n = out_sz - 1;
        memcpy(out, resolved, n);
        out[n] = '\0';
    } else {
        size_t n = strlen(in);
        if (n >= out_sz) n = out_sz - 1;
        memcpy(out, in, n);
        out[n] = '\0';
    }
#endif
}

/* ================================================================== */
/*  Directory walk                                                    */
/* ================================================================== */

#if defined(_WIN32)

int wisp_walk_dir(const char *root, wisp_walk_cb cb, void *user) {
    /* Windows walker: FindFirstFileA / FindNextFileA, recursive.
     * Paths use forward slashes in the callback so that downstream
     * code (which expects POSIX-style separators) sees a consistent
     * shape. */
    char pattern[MAX_PATH * 2];
    snprintf(pattern, sizeof(pattern), "%s\\*", root);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;

    do {
        if (strcmp(fd.cFileName, ".") == 0 ||
            strcmp(fd.cFileName, "..") == 0) continue;

        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        int rc = cb(fd.cFileName, is_dir, user);
        if (rc != 0) break;
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return 0;
}

#else  /* POSIX */

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

int wisp_walk_dir(const char *root, wisp_walk_cb cb, void *user) {
    DIR *d = opendir(root);
    if (!d) return -1;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 ||
            strcmp(e->d_name, "..") == 0) continue;

        char child[MAX_PATH_LEN * 2];
        snprintf(child, sizeof(child), "%s/%s", root, e->d_name);

        struct stat st;
        if (lstat(child, &st) != 0) continue;

        int is_dir = S_ISDIR(st.st_mode);
        int rc = cb(e->d_name, is_dir, user);
        if (rc != 0) break;
    }

    closedir(d);
    return 0;
}

#endif /* _WIN32 */

/* ================================================================== */
/*  Subprocess capture                                                */
/* ================================================================== */

#if defined(_WIN32)

char *wisp_run_capture(const char *cmd, int *exit_code) {
    if (exit_code) *exit_code = -1;

    /* Prefix the command so cmd.exe treats it as a shell command. */
    char full[8192];
    snprintf(full, sizeof(full), "cmd.exe /c %s 2>&1", cmd);

    FILE *p = _popen(full, "rb");
    if (!p) return NULL;

    size_t cap = 16384, used = 0;
    char *buf = malloc(cap);
    if (!buf) { _pclose(p); return NULL; }

    for (;;) {
        if (used + 4096 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
        size_t r = fread(buf + used, 1, 4096, p);
        if (r == 0) break;
        used += r;
    }
    buf[used] = '\0';

    int rc = _pclose(p);
    if (exit_code) *exit_code = rc;
    return buf;
}

#else  /* POSIX */

#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

char *wisp_run_capture(const char *cmd, int *exit_code) {
    if (exit_code) *exit_code = -1;

    int pipefd[2];
    if (pipe(pipefd) != 0) return NULL;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return NULL;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        const char *shell = wisp_shell_path();
        execl(shell, "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);

    size_t cap = 16384, used = 0;
    char *buf = malloc(cap);
    if (!buf) { close(pipefd[0]); return NULL; }

    for (;;) {
        if (used + 4096 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
        ssize_t r = read(pipefd[0], buf + used, 4096);
        if (r <= 0) break;
        used += (size_t)r;
    }
    buf[used] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (exit_code) *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return buf;
}

#endif /* _WIN32 */