// language: C, file: wisp-code/platform.c
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/*  Compile-time platform tag                                        */
/* ================================================================== */

#if defined(_WIN32) || defined(_WIN64)
    #define AX_PLATFORM "windows"
#elif defined(__APPLE__) && defined(__MACH__)
    #define AX_PLATFORM "macos"
#elif defined(__linux__)
    #if defined(__ANDROID__)
        #define AX_PLATFORM "termux"
    #else
        #define AX_PLATFORM "linux"
    #endif
#else
    #define AX_PLATFORM "unknown"
#endif

/* ================================================================== */
/*  Architecture                                                      */
/* ================================================================== */

static const char *arch_name(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
    return "armv7";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "i386";
#elif defined(__riscv) && (__riscv_xlen == 64)
    return "riscv64";
#elif defined(__powerpc64__)
    return "ppc64";
#else
    return "unknown";
#endif
}

/* ================================================================== */
/*  Append helpers — never overflow, always NUL-terminate             */
/* ================================================================== */

static void append(char *buf, size_t buf_sz, size_t *used, const char *s) {
    if (!s || !s[0]) return;
    if (*used >= buf_sz - 1) return;
    int n = snprintf(buf + *used, buf_sz - *used, "%s", s);
    if (n < 0) return;
    if ((size_t)n >= buf_sz - *used) {
        *used = buf_sz - 1;   /* truncated */
        return;
    }
    *used += (size_t)n;
}

static void append_field(char *buf, size_t buf_sz, size_t *used,
                         const char *key, const char *val) {
    if (!val || !val[0]) return;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), " %s=%s", key, val);
    append(buf, buf_sz, used, tmp);
}

/* Trim leading/trailing whitespace and quotes in place; return pointer
 * to the trimmed region. Modifies `s`. */
static char *trim_inplace(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '"' || *s == '\'') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
                     s[n-1] == '"' || s[n-1] == '\'' ||
                     s[n-1] == '\n' || s[n-1] == '\r')) {
        s[--n] = '\0';
    }
    return s;
}

/* ================================================================== */
/*  Linux (glibc/musl): /etc/os-release + kernel release             */
/* ================================================================== */

#if defined(__linux__) && !defined(__ANDROID__)

static void probe_os_release(char *id, size_t id_sz,
                             char *ver, size_t ver_sz,
                             char *pretty, size_t pretty_sz) {
    id[0] = ver[0] = pretty[0] = '\0';

    FILE *f = fopen("/etc/os-release", "r");
    if (!f) f = fopen("/usr/lib/os-release", "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim_inplace(line);
        char *v = trim_inplace(eq + 1);

        if (strcmp(k, "ID") == 0 && id_sz > 0) {
            snprintf(id, id_sz, "%s", v);
        } else if (strcmp(k, "VERSION_ID") == 0 && ver_sz > 0) {
            snprintf(ver, ver_sz, "%s", v);
        } else if (strcmp(k, "PRETTY_NAME") == 0 && pretty_sz > 0) {
            snprintf(pretty, pretty_sz, "%s", v);
        }
    }
    fclose(f);

    /* If VERSION_ID was missing but PRETTY_NAME looks like
     * "Ubuntu 24.04.1 LTS", extract the first digit-leading token. */
    if (!ver[0] && pretty[0]) {
        const char *p = pretty;
        const char *last = NULL;
        while (*p) {
            if (*p >= '0' && *p <= '9') { last = p; break; }
            p++;
        }
        if (last) {
            size_t i = 0;
            while (last[i] && last[i] != ' ' && last[i] != '\n' &&
                   i < ver_sz - 1) {
                ver[i] = last[i];
                i++;
            }
            ver[i] = '\0';
        }
    }
}

static void probe_kernel(char *buf, size_t buf_sz) {
    buf[0] = '\0';
    FILE *f = fopen("/proc/sys/kernel/osrelease", "r");
    if (!f) return;
    if (fgets(buf, (int)buf_sz, f)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    }
    fclose(f);
}

#endif /* __linux__ && !__ANDROID__ */

/* ================================================================== */
/*  Android / Termux                                                  */
/* ================================================================== */

#if defined(__linux__) && defined(__ANDROID__)

/* Termux: __ANDROID__ is set, but a plain Android NDK build is too.
 * Distinguish by the environment Termux guarantees: PREFIX points
 * under /data/data/com.termux/, or TERMUX_VERSION is set. */
static int probe_termux(char *ver_out, size_t ver_sz) {
    const char *prefix = getenv("PREFIX");
    const char *tv     = getenv("TERMUX_VERSION");

    int is_termux = 0;
    if (prefix && strstr(prefix, "/com.termux/")) is_termux = 1;
    if (tv && tv[0]) is_termux = 1;
    if (!is_termux) return 0;

    ver_out[0] = '\0';
    if (tv && tv[0]) {
        snprintf(ver_out, ver_sz, "%s", tv);
    }
    return 1;
}

static void probe_android_release(char *buf, size_t buf_sz) {
    buf[0] = '\0';
    /* Android sets ro.build.version.release in getprop output; without
     * spawning getprop, /system/build.prop is the next best thing. */
    FILE *f = fopen("/system/build.prop", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "ro.build.version.release=", 25) == 0) {
            char *v = trim_inplace(line + 25);
            snprintf(buf, buf_sz, "%s", v);
            break;
        }
    }
    fclose(f);
}

#endif /* __linux__ && __ANDROID__ */

/* ================================================================== */
/*  macOS: SystemVersion.plist                                        */
/* ================================================================== */

#if defined(__APPLE__) && defined(__MACH__)

static void probe_macos_version(char *buf, size_t buf_sz) {
    buf[0] = '\0';
    FILE *f = fopen("/System/Library/CoreServices/SystemVersion.plist", "r");
    if (!f) return;

    char line[1024];
    int want_next = 0;
    while (fgets(line, sizeof(line), f)) {
        if (want_next) {
            /* <string>14.5</string> */
            char *lt = strchr(line, '>');
            char *gt = lt ? strchr(lt + 1, '<') : NULL;
            if (lt && gt && gt > lt + 1) {
                size_t n = (size_t)(gt - (lt + 1));
                if (n >= buf_sz) n = buf_sz - 1;
                memcpy(buf, lt + 1, n);
                buf[n] = '\0';
                break;
            }
            want_next = 0;
        }
        if (strstr(line, "<key>ProductVersion</key>")) {
            want_next = 1;
        }
    }
    fclose(f);
}

#endif /* __APPLE__ */

/* ================================================================== */
/*  Windows: RtlGetVersion via ntdll                                  */
/* ================================================================== */

#if defined(_WIN32) || defined(_WIN64)

#include <windows.h>

typedef LONG (WINAPI *RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

static void probe_windows_version(char *buf, size_t buf_sz) {
    buf[0] = '\0';

    HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
    if (!hMod) return;
    RtlGetVersionPtr fn =
        (RtlGetVersionPtr)(void *)GetProcAddress(hMod, "RtlGetVersion");
    if (!fn) return;

    RTL_OSVERSIONINFOW vi;
    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return;

    snprintf(buf, buf_sz, "%lu.%lu.%lu",
             (unsigned long)vi.dwMajorVersion,
             (unsigned long)vi.dwMinorVersion,
             (unsigned long)vi.dwBuildNumber);
}

#endif /* _WIN32 */

/* ================================================================== */
/*  Public                                                            */
/* ================================================================== */

void platform_describe(char *buf, size_t buf_sz) {
    if (!buf || buf_sz < 2) return;
    buf[0] = '\0';

    size_t used = 0;
    append(buf, buf_sz, &used, "[ENV] platform=");
    append(buf, buf_sz, &used, AX_PLATFORM);

#if defined(__linux__)
    #if defined(__ANDROID__)
        /* Termux: report android release + termux version if present */
        {
            char tv[64];
            int is_tx = probe_termux(tv, sizeof(tv));
            char av[64];
            probe_android_release(av, sizeof(av));
            if (av[0]) append_field(buf, buf_sz, &used, "android", av);
            if (is_tx && tv[0]) {
                append_field(buf, buf_sz, &used, "termux", tv);
            }
        }
    #else
        {
            char id[64], ver[64], pretty[256];
            probe_os_release(id, sizeof(id), ver, sizeof(ver),
                             pretty, sizeof(pretty));
            if (id[0])   append_field(buf, buf_sz, &used, "distro", id);
            if (ver[0])  append_field(buf, buf_sz, &used, "version", ver);

            char kern[128];
            probe_kernel(kern, sizeof(kern));
            if (kern[0]) append_field(buf, buf_sz, &used, "kernel", kern);
        }
    #endif
#elif defined(__APPLE__) && defined(__MACH__)
    {
        char ver[64];
        probe_macos_version(ver, sizeof(ver));
        if (ver[0]) append_field(buf, buf_sz, &used, "version", ver);
    }
#elif defined(_WIN32) || defined(_WIN64)
    {
        char ver[64];
        probe_windows_version(ver, sizeof(ver));
        if (ver[0]) append_field(buf, buf_sz, &used, "version", ver);
    }
#endif

    append_field(buf, buf_sz, &used, "arch", arch_name());
}