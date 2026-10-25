// language: C, file: wisp-code/theme.h
#ifndef THEME_H
#define THEME_H

#include <stddef.h>

/* Theme slots. Each slot maps to one RGB triple in a theme_t.
 * The names mirror the CSS custom properties in the theme files
 * so a new theme is a mechanical transcription. */
typedef enum {
    TH_BG = 0,          /* body background */
    TH_BORDER,          /* frame, box drawing */
    TH_SPLIT,           /* the one vertical + its tees */
    TH_TITLE,           /* top-rule title text */
    TH_ACCENT,          /* primary accent — headings, live signal */
    TH_PROMPT,          /* input prompt "> " */
    TH_PATH,            /* project root in the title */
    TH_OK,              /* status: ready, build OK, applied */
    TH_ERR,             /* status: error, failed */
    TH_WARN,            /* status: busy, thinking, waiting */
    TH_INFO,            /* status: informational */
    TH_MUTED,           /* placeholder, disabled, dim chrome */
    TH_USER,            /* chat: user lines */
    TH_AI,              /* chat: assistant lines */
    TH_SYSTEM,          /* chat: bracketed system notices */
    TH_LINENO,          /* OUTPUT panel line numbers */
    TH_HINT,            /* bottom hint row, panel header idle */
    TH_COUNT
} theme_slot_t;

typedef struct {
    const char *name;                   /* "wisp", "cyber", ... */
    const char *desc;                   /* one-line human description */
    unsigned char rgb[TH_COUNT][3];
} theme_t;

/* Returns the number of themes. */
int theme_count(void);

/* Returns the theme at index, or NULL if out of range. */
const theme_t *theme_at(int idx);

/* Returns the index of the named theme, or -1. */
int theme_index_of(const char *name);

/* Currently selected theme index. */
int theme_current_index(void);
const theme_t *theme_current(void);

/* Select by index or name. Returns 0 on success. */
int theme_select_index(int idx);
int theme_select_name(const char *name);

/* Cycle to the next theme. Returns the new index. */
int theme_next(void);

/* Pin the fallback theme (by name) so reordering THEMES[] can't
 * silently change which theme appears when no wisp-code.conf exists.
 * Idempotent. Call once before theme_load(). */
void theme_init(void);

/* Write an ANSI SGR sequence for `slot` into `buf` (foreground).
 * `buf` must be at least 16 bytes. Returns bytes written.
 * If the theme system is disabled (wisp_NO_COLOR), returns 0. */
int theme_fg(theme_slot_t slot, char *buf, size_t buf_sz);

/* Write an ANSI SGR sequence for `slot` as background. */
int theme_bg(theme_slot_t slot, char *buf, size_t buf_sz);

/* Load/save the selected theme name from/to
 * <project_root>/wisp-code.conf. Missing file or missing key is
 * not an error — the default is used. save is best-effort. */
void theme_load(const char *project_root);
void theme_save(const char *project_root);

/* Accessor used by skin.h's C_* macros. Rotates through four
 * internal buffers so a caller may hold several of these live
 * at once. Returns "" under wisp_NO_COLOR. */
const char *skin_slot(theme_slot_t slot);

#endif