// language: C, file: wisp-code/platform.h
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>

/* Fill `buf` with a single-line environment summary, e.g.:
 *
 *   [ENV] platform=linux distro=ubuntu version=24.04 kernel=6.8.0 arch=x86_64
 *   [ENV] platform=macos version=14.5 arch=arm64
 *   [ENV] platform=windows version=10.0.22631 arch=x86_64
 *   [ENV] platform=termux android=13 termux=0.118 arch=aarch64
 *
 * Never fails: on any probe error the corresponding field is omitted.
 * Always NUL-terminates. Safe to call once at startup and cache. */
void platform_describe(char *buf, size_t buf_sz);

#endif /* wisp_PLATFORM_H */