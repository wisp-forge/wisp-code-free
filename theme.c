// language: C, file: wisp-code/theme.c
#include "theme.h"
#include "console.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ================================================================== */
/*  The table                                                         */
/* ================================================================== */

#ifdef RGB
#undef RGB
#endif
#define RGB(r,g,b) {r,g,b}

static const theme_t THEMES[] = {

/* ---- wisp (formerly "aiden") ------------------------------------- *
 * The original Han blue + Han purple skin.                          */
{
    "wisp",
    "Han blue + Han purple. The signature.",
    {
        [TH_BG]      = RGB( 2,  4, 12),
        [TH_BORDER]  = RGB(24, 36, 92),
        [TH_SPLIT]   = RGB(31, 46,114),
        [TH_TITLE]   = RGB(110, 78,180),
        [TH_ACCENT]  = RGB(68,108,207),
        [TH_PROMPT]  = RGB(110, 78,180),
        [TH_PATH]    = RGB( 90,110,160),
        [TH_OK]      = RGB( 95,211,190),
        [TH_ERR]     = RGB(200, 80,130),
        [TH_WARN]    = RGB(180,150,200),
        [TH_INFO]    = RGB(180,185,215),
        [TH_MUTED]   = RGB( 90, 95,110),
        [TH_USER]    = RGB( 68,108,207),
        [TH_AI]      = RGB(196,181,253),
        [TH_SYSTEM]  = RGB(120,130,180),
        [TH_LINENO]  = RGB( 90,100,180),
        [TH_HINT]    = RGB(120,130,180),
    }
},

/* ---- arctic ------------------------------------------------------ */
{
    "arctic",
    "Icy pale blues. High key, low saturation.",
    {
        [TH_BG]      = RGB(237,243,248),
        [TH_BORDER]  = RGB(180,205,224),
        [TH_SPLIT]   = RGB(150,180,205),
        [TH_TITLE]   = RGB( 42,111,168),
        [TH_ACCENT]  = RGB( 42,111,168),
        [TH_PROMPT]  = RGB( 42,111,168),
        [TH_PATH]    = RGB(120,168,200),
        [TH_OK]      = RGB( 42,111,168),
        [TH_ERR]     = RGB(196, 58, 63),
        [TH_WARN]    = RGB(168,106, 42),
        [TH_INFO]    = RGB( 90,122,148),
        [TH_MUTED]   = RGB(150,170,184),
        [TH_USER]    = RGB( 42,111,168),
        [TH_AI]      = RGB( 74,143,168),
        [TH_SYSTEM]  = RGB(120,148,168),
        [TH_LINENO]  = RGB(150,180,205),
        [TH_HINT]    = RGB( 90,122,148),
    }
},

/* ---- cyber ------------------------------------------------------- */
{
    "cyber",
    "Hot pink + neon green on near-black.",
    {
        [TH_BG]      = RGB( 10,  4, 16),
        [TH_BORDER]  = RGB(176, 42,107),
        [TH_SPLIT]   = RGB(255, 61,154),
        [TH_TITLE]   = RGB(255,123,190),
        [TH_ACCENT]  = RGB(255, 61,154),
        [TH_PROMPT]  = RGB(255,123,190),
        [TH_PATH]    = RGB(154,128,168),
        [TH_OK]      = RGB( 57,255,136),
        [TH_ERR]     = RGB(229, 72, 77),
        [TH_WARN]    = RGB(255,170, 61),
        [TH_INFO]    = RGB(232,216,240),
        [TH_MUTED]   = RGB( 94, 74,106),
        [TH_USER]    = RGB(255, 61,154),
        [TH_AI]      = RGB( 57,255,136),
        [TH_SYSTEM]  = RGB(154,128,168),
        [TH_LINENO]  = RGB(176, 42,107),
        [TH_HINT]    = RGB(154,128,168),
    }
},

/* ---- default ----------------------------------------------------- */
{
    "default",
    "Neutral cool grey. Single accent, no glow.",
    {
        [TH_BG]      = RGB( 15, 17, 21),
        [TH_BORDER]  = RGB( 46, 58,110),
        [TH_SPLIT]   = RGB( 90,107,216),
        [TH_TITLE]   = RGB(165,177,255),
        [TH_ACCENT]  = RGB(124,140,255),
        [TH_PROMPT]  = RGB(165,177,255),
        [TH_PATH]    = RGB(154,161,173),
        [TH_OK]      = RGB(124,140,255),
        [TH_ERR]     = RGB(229, 72, 77),
        [TH_WARN]    = RGB(247,140,108),
        [TH_INFO]    = RGB(230,232,236),
        [TH_MUTED]   = RGB( 90, 97,110),
        [TH_USER]    = RGB(165,177,255),
        [TH_AI]      = RGB(230,232,236),
        [TH_SYSTEM]  = RGB(154,161,173),
        [TH_LINENO]  = RGB( 90,107,216),
        [TH_HINT]    = RGB(154,161,173),
    }
},

/* ---- ember ------------------------------------------------------- */
{
    "ember",
    "Warm brown + amber. Firelight.",
    {
        [TH_BG]      = RGB( 18, 10,  5),
        [TH_BORDER]  = RGB( 94, 56, 32),
        [TH_SPLIT]   = RGB(176,104, 64),
        [TH_TITLE]   = RGB(255,187,136),
        [TH_ACCENT]  = RGB(255,154, 92),
        [TH_PROMPT]  = RGB(255,187,136),
        [TH_PATH]    = RGB(168,138,112),
        [TH_OK]      = RGB(168,230,160),
        [TH_ERR]     = RGB(229, 72, 77),
        [TH_WARN]    = RGB(255,209,102),
        [TH_INFO]    = RGB(232,213,192),
        [TH_MUTED]   = RGB(110, 88, 68),
        [TH_USER]    = RGB(255,154, 92),
        [TH_AI]      = RGB(255,209,102),
        [TH_SYSTEM]  = RGB(168,138,112),
        [TH_LINENO]  = RGB( 94, 56, 32),
        [TH_HINT]    = RGB(168,138,112),
    }
},

/* ---- forest ------------------------------------------------------ */
{
    "forest",
    "Deep green + sage. Quiet, organic.",
    {
        [TH_BG]      = RGB(  7, 16, 11),
        [TH_BORDER]  = RGB( 38, 84, 54),
        [TH_SPLIT]   = RGB( 74,143, 99),
        [TH_TITLE]   = RGB(168,239,191),
        [TH_ACCENT]  = RGB(123,224,160),
        [TH_PROMPT]  = RGB(168,239,191),
        [TH_PATH]    = RGB(127,151,132),
        [TH_OK]      = RGB(123,224,160),
        [TH_ERR]     = RGB(229, 72, 77),
        [TH_WARN]    = RGB(224,200,123),
        [TH_INFO]    = RGB(210,226,212),
        [TH_MUTED]   = RGB( 78, 95, 82),
        [TH_USER]    = RGB(123,224,160),
        [TH_AI]      = RGB(200,217,184),
        [TH_SYSTEM]  = RGB(127,151,132),
        [TH_LINENO]  = RGB( 38, 84, 54),
        [TH_HINT]    = RGB(127,151,132),
    }
},

/* ---- midnight ---------------------------------------------------- */
{
    "midnight",
    "Deep navy, cyan + amber. Technical.",
    {
        [TH_BG]      = RGB(  5,  8, 16),
        [TH_BORDER]  = RGB( 31, 79, 96),
        [TH_SPLIT]   = RGB( 58,143,168),
        [TH_TITLE]   = RGB(143,228,255),
        [TH_ACCENT]  = RGB( 95,211,243),
        [TH_PROMPT]  = RGB(143,228,255),
        [TH_PATH]    = RGB(122,140,163),
        [TH_OK]      = RGB(168,230,160),
        [TH_ERR]     = RGB(229, 72, 77),
        [TH_WARN]    = RGB(240,178,122),
        [TH_INFO]    = RGB(213,226,240),
        [TH_MUTED]   = RGB( 74, 90,112),
        [TH_USER]    = RGB( 95,211,243),
        [TH_AI]      = RGB(240,178,122),
        [TH_SYSTEM]  = RGB(122,140,163),
        [TH_LINENO]  = RGB( 31, 79, 96),
        [TH_HINT]    = RGB(122,140,163),
    }
},

/* ---- mono -------------------------------------------------------- */
{
    "mono",
    "Pure greyscale. Contrast does the work.",
    {
        [TH_BG]      = RGB( 10, 10, 12),
        [TH_BORDER]  = RGB( 62, 64, 70),
        [TH_SPLIT]   = RGB(122,125,132),
        [TH_TITLE]   = RGB(232,233,236),
        [TH_ACCENT]  = RGB(201,203,209),
        [TH_PROMPT]  = RGB(232,233,236),
        [TH_PATH]    = RGB(138,141,148),
        [TH_OK]      = RGB(232,233,236),
        [TH_ERR]     = RGB(229, 72, 77),
        [TH_WARN]    = RGB(216,217,221),
        [TH_INFO]    = RGB(216,217,221),
        [TH_MUTED]   = RGB( 84, 86, 92),
        [TH_USER]    = RGB(255,255,255),
        [TH_AI]      = RGB(184,188,194),
        [TH_SYSTEM]  = RGB(138,141,148),
        [TH_LINENO]  = RGB( 62, 64, 70),
        [TH_HINT]    = RGB(138,141,148),
    }
},

/* ---- ocean ------------------------------------------------------- */
{
    "ocean",
    "Deep blue, seafoam + coral. Tide pool.",
    {
        [TH_BG]      = RGB(  4, 11, 20),
        [TH_BORDER]  = RGB( 30, 81, 72),
        [TH_SPLIT]   = RGB( 58,142,130),
        [TH_TITLE]   = RGB(143,232,216),
        [TH_ACCENT]  = RGB(102,217,196),
        [TH_PROMPT]  = RGB(143,232,216),
        [TH_PATH]    = RGB(122,148,168),
        [TH_OK]      = RGB(168,230,160),
        [TH_ERR]     = RGB(229, 72, 77),
        [TH_WARN]    = RGB(255,184,108),
        [TH_INFO]    = RGB(200,219,232),
        [TH_MUTED]   = RGB( 72, 92,110),
        [TH_USER]    = RGB(102,217,196),
        [TH_AI]      = RGB(255,139,123),
        [TH_SYSTEM]  = RGB(122,148,168),
        [TH_LINENO]  = RGB( 30, 81, 72),
        [TH_HINT]    = RGB(122,148,168),
    }
},

/* ---- paper ------------------------------------------------------- */
{
    "paper",
    "Warm off-white, ink brown + teal. Reading theme.",
    {
        [TH_BG]      = RGB(245,240,232),
        [TH_BORDER]  = RGB(216,200,176),
        [TH_SPLIT]   = RGB(191,160,128),
        [TH_TITLE]   = RGB(138, 90, 42),
        [TH_ACCENT]  = RGB(138, 90, 42),
        [TH_PROMPT]  = RGB(138, 90, 42),
        [TH_PATH]    = RGB(122,106, 84),
        [TH_OK]      = RGB( 58,106,106),
        [TH_ERR]     = RGB(196, 58, 63),
        [TH_WARN]    = RGB(168, 90, 42),
        [TH_INFO]    = RGB( 58, 46, 32),
        [TH_MUTED]   = RGB(168,152,128),
        [TH_USER]    = RGB(138, 90, 42),
        [TH_AI]      = RGB( 58,106,106),
        [TH_SYSTEM]  = RGB(122,106, 84),
        [TH_LINENO]  = RGB(191,160,128),
        [TH_HINT]    = RGB(122,106, 84),
    }
},

/* ---- sunset ------------------------------------------------------ */
{
    "sunset",
    "Warm dark, peach + coral. End of day.",
    {
        [TH_BG]      = RGB( 21, 10, 14),
        [TH_BORDER]  = RGB(110, 70, 56),
        [TH_SPLIT]   = RGB(184,122, 98),
        [TH_TITLE]   = RGB(255,196,172),
        [TH_ACCENT]  = RGB(245,168,139),
        [TH_PROMPT]  = RGB(255,196,172),
        [TH_PATH]    = RGB(168,142,136),
        [TH_OK]      = RGB(195,232,141),
        [TH_ERR]     = RGB(229, 72, 77),
        [TH_WARN]    = RGB(255,203,107),
        [TH_INFO]    = RGB(240,216,208),
        [TH_MUTED]   = RGB(110, 84, 80),
        [TH_USER]    = RGB(245,168,139),
        [TH_AI]      = RGB(232,153,199),
        [TH_SYSTEM]  = RGB(168,142,136),
        [TH_LINENO]  = RGB(110, 70, 56),
        [TH_HINT]    = RGB(168,142,136),
    }
},

/* ---- vapor ------------------------------------------------------- */
{
    "vapor",
    "Synthwave pastel on dark navy. Dreamy.",
    {
        [TH_BG]      = RGB( 10, 10, 30),
        [TH_BORDER]  = RGB( 94, 48, 72),
        [TH_SPLIT]   = RGB(176, 94,136),
        [TH_TITLE]   = RGB(255,176,220),
        [TH_ACCENT]  = RGB(255,142,200),
        [TH_PROMPT]  = RGB(255,176,220),
        [TH_PATH]    = RGB(138,130,176),
        [TH_OK]      = RGB(168,230,160),
        [TH_ERR]     = RGB(229, 72, 77),
        [TH_WARN]    = RGB(255,203,107),
        [TH_INFO]    = RGB(216,212,240),
        [TH_MUTED]   = RGB( 90, 84,120),
        [TH_USER]    = RGB(255,142,200),
        [TH_AI]      = RGB(180,160,255),
        [TH_SYSTEM]  = RGB(138,130,176),
        [TH_LINENO]  = RGB( 94, 48, 72),
        [TH_HINT]    = RGB(138,130,176),
    }
},

};

#undef RGB

#define THEME_COUNT ((int)(sizeof(THEMES) / sizeof(THEMES[0])))

/* ================================================================== */
/*  State                                                             */
/* ================================================================== */

static int g_current = 0;
static int g_initialized = 0;

/* Pin the fallback by name so reordering THEMES[] can't silently
 * change which theme appears when no wisp-code.conf exists. */
void theme_init(void) {
    if (g_initialized) return;
    g_initialized = 1;
    int idx = theme_index_of("default");
    if (idx >= 0) g_current = idx;
}

/* ================================================================== */
/*  Lookup                                                            */
/* ================================================================== */

int theme_count(void) { return THEME_COUNT; }

const theme_t *theme_at(int idx) {
    if (idx < 0 || idx >= THEME_COUNT) return NULL;
    return &THEMES[idx];
}

int theme_index_of(const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < THEME_COUNT; i++) {
        if (strcmp(THEMES[i].name, name) == 0) return i;
    }
    return -1;
}

int theme_current_index(void) { return g_current; }

const theme_t *theme_current(void) { return &THEMES[g_current]; }

int theme_select_index(int idx) {
    if (idx < 0 || idx >= THEME_COUNT) return -1;
    g_current = idx;
    return 0;
}

int theme_select_name(const char *name) {
    int idx = theme_index_of(name);
    if (idx < 0) return -1;
    g_current = idx;
    return 0;
}

int theme_next(void) {
    g_current = (g_current + 1) % THEME_COUNT;
    return g_current;
}

/* ================================================================== */
/*  Terminal color-depth detection                                    */
/*                                                                    */
/*  Four tiers:                                                       */
/*    0 = no color  (TERM=dumb, or TERM unset and no COLORTERM)      */
/*    1 = 8/16 ANSI (bare Linux console, vt100, older xterm)         */
/*    2 = 256 color (most modern terminals, tmux, screen)            */
/*    3 = 24-bit truecolor (kitty, iTerm2, WezTerm, recent xterm)    */
/*                                                                    */
/*  The result is cached after the first call — the environment does */
/*  not change during the process lifetime.                          */
/* ================================================================== */

static int g_color_depth = -1;

static int terminal_color_depth(void) {
    if (g_color_depth >= 0) return g_color_depth;

#if defined(_WIN32)
    /* Windows Terminal and ConPTY support truecolor natively. The
     * classic conhost also supports ANSI sequences when
     * ENABLE_VIRTUAL_TERMINAL_PROCESSING is set (which tui_init
     * does), but only 16 colors. Detect by checking for Windows
     * Terminal's environment marker. */
    const char *wt = getenv("WT_SESSION");
    if (wt && wt[0]) {
        g_color_depth = 3;
        return 3;
    }
    /* Fallback: assume 24-bit. Modern Windows consoles support it,
     * and if they don't, ANSI sequences are still interpreted (they
     * just get mapped to the nearest of the 16 palette colors). */
    g_color_depth = 3;
    return 3;

#else

    const char *ct = getenv("COLORTERM");
    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) {
        g_color_depth = 3;
        return 3;
    }

    const char *term = getenv("TERM");
    if (!term || !term[0]) {
        g_color_depth = 0;
        return 0;
    }
    if (strcmp(term, "dumb") == 0) {
        g_color_depth = 0;
        return 0;
    }

    /* Bare Linux console — the VT only understands the base 16. */
    if (strcmp(term, "linux") == 0) {
        g_color_depth = 1;
        return 1;
    }

    /* Terminals that advertise 256-color capability. */
    if (strstr(term, "256color")) {
        g_color_depth = 2;
        return 2;
    }

    /* xterm-family, screen, tmux — assume at least 256. */
    if (strstr(term, "xterm") ||
        strstr(term, "screen") ||
        strstr(term, "tmux") ||
        strstr(term, "rxvt")) {
        g_color_depth = 2;
        return 2;
    }

    /* Unknown TERM: default to 16-color, the safest nonzero assumption. */
    g_color_depth = 1;
    return 1;
#endif
}

/* ================================================================== */
/*  RGB -> terminal color conversions                                 */
/* ================================================================== */

/* Nearest entry in the xterm-256 color cube. Used for depth 2. */
static int rgb_to_256(unsigned char r, unsigned char g, unsigned char b) {
    static const int steps[6] = {0, 95, 135, 175, 215, 255};
    int best_d = 1 << 30;
    int ri = 0, gi = 0, bi = 0;

    for (int i = 0; i < 6; i++) {
        int dr = (int)r - steps[i]; dr *= dr;
        if (dr >= best_d) continue;
        for (int j = 0; j < 6; j++) {
            int dg = (int)g - steps[j]; dg *= dg;
            int drg = dr + dg;
            if (drg >= best_d) continue;
            for (int k = 0; k < 6; k++) {
                int db = (int)b - steps[k]; db *= db;
                int d = drg + db;
                if (d < best_d) {
                    best_d = d;
                    ri = i; gi = j; bi = k;
                }
            }
        }
    }
    return 16 + 36 * ri + 6 * gi + bi;
}

/* Nearest of the 8 base ANSI colors. Used for depth 1.
 * Returns 0..7: 0=black, 1=red, 2=green, 3=yellow, 4=blue, 5=magenta,
 * 6=cyan, 7=white. */
static int rgb_to_ansi8(unsigned char r, unsigned char g, unsigned char b) {
    int max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int v   = max;
    int sat = (max == 0) ? 0 : (max - min);

    /* Very dark -> black. Very desaturated -> black or white by
     * brightness. Thresholds chosen to look reasonable on the
     * default `linux` console palette. */
    if (v < 48) return 0;
    if (sat < 32) return (v > 160) ? 7 : 0;

    /* Hue bucket. Cheap approximation: compare pairwise. */
    if (r >= g && r >= b) {
        return (g > b) ? 3 : 5;         /* yellow or magenta */
    } else if (g >= r && g >= b) {
        return (r > b) ? 3 : 6;         /* yellow or cyan */
    } else {
        return (r > g) ? 5 : 6;         /* magenta or cyan */
    }
}

/* ================================================================== */
/*  ANSI sequence generation                                          */
/* ================================================================== */

int theme_fg(theme_slot_t slot, char *buf, size_t buf_sz) {
#ifdef WISP_NO_COLOR
    (void)slot; (void)buf; (void)buf_sz;
    return 0;
#else
    if (!buf || buf_sz < 16) return 0;
    if (slot < 0 || slot >= TH_COUNT) return 0;

    int depth = terminal_color_depth();
    if (depth == 0) { buf[0] = '\0'; return 0; }

    const unsigned char *c = THEMES[g_current].rgb[slot];
    int n;

    switch (depth) {
        case 3:
            n = snprintf(buf, buf_sz, "\x1b[38;2;%d;%d;%dm",
                         c[0], c[1], c[2]);
            break;
        case 2:
            n = snprintf(buf, buf_sz, "\x1b[38;5;%dm",
                         rgb_to_256(c[0], c[1], c[2]));
            break;
        case 1:
        default: {
            int idx = rgb_to_ansi8(c[0], c[1], c[2]);
            n = snprintf(buf, buf_sz, "\x1b[3%dm", idx);
            break;
        }
    }
    return n > 0 ? n : 0;
#endif
}

int theme_bg(theme_slot_t slot, char *buf, size_t buf_sz) {
#ifdef WISP_NO_COLOR
    (void)slot; (void)buf; (void)buf_sz;
    return 0;
#else
    if (!buf || buf_sz < 16) return 0;
    if (slot < 0 || slot >= TH_COUNT) return 0;

    int depth = terminal_color_depth();
    if (depth == 0) { buf[0] = '\0'; return 0; }

    const unsigned char *c = THEMES[g_current].rgb[slot];
    int n;

    switch (depth) {
        case 3:
            n = snprintf(buf, buf_sz, "\x1b[48;2;%d;%d;%dm",
                         c[0], c[1], c[2]);
            break;
        case 2:
            n = snprintf(buf, buf_sz, "\x1b[48;5;%dm",
                         rgb_to_256(c[0], c[1], c[2]));
            break;
        case 1:
        default: {
            int idx = rgb_to_ansi8(c[0], c[1], c[2]);
            n = snprintf(buf, buf_sz, "\x1b[4%dm", idx);
            break;
        }
    }
    return n > 0 ? n : 0;
#endif
}

/* ================================================================== */
/*  Skin slot accessor                                                */
/*                                                                    */
/*  skin.h's C_* macros expand to skin_slot(SLOT). Each call rotates  */
/*  through a ring of four buffers so a caller can hold two or three  */
/*  of these live at once (e.g. C_BOLD then C_ERR then text) without  */
/*  one overwriting another mid-write.                                */
/* ================================================================== */

const char *skin_slot(theme_slot_t slot) {
#ifdef WISP_NO_COLOR
    (void)slot;
    return "";
#else
    static char bufs[4][24];
    static int  next = 0;
    char *b = bufs[next];
    next = (next + 1) & 3;
    if (theme_fg(slot, b, sizeof(bufs[0])) == 0) b[0] = '\0';
    return b;
#endif
}

/* ================================================================== */
/*  Persistence — wisp-code.conf                                     */
/*                                                                    */
/*  Format: one `key = value` per line. We only read/write `theme`.   */
/*  Unknown keys are preserved on save. Lines starting with # are     */
/*  comments. This is deliberately tiny — the file is expected to     */
/*  hold a handful of keys, not a config language.                    */
/* ================================================================== */

void theme_load(const char *project_root) {
    if (!project_root || !project_root[0]) return;

    char path[MAX_PATH_LEN * 2 + 32];
    snprintf(path, sizeof(path), "%s/wisp-code.conf", project_root);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = p;
        char *v = eq + 1;
        char *ke = k + strlen(k);
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = '\0';
        while (*v == ' ' || *v == '\t') v++;
        char *ve = v + strlen(v);
        while (ve > v && (ve[-1] == '\n' || ve[-1] == '\r' ||
                          ve[-1] == ' '  || ve[-1] == '\t')) *--ve = '\0';

        if (strcmp(k, "theme") == 0) {
            theme_select_name(v);
        }
    }
    fclose(f);
}

void theme_save(const char *project_root) {
    if (!project_root || !project_root[0]) return;

    char path[MAX_PATH_LEN * 2 + 32];
    snprintf(path, sizeof(path), "%s/wisp-code.conf", project_root);

    char **lines = NULL;
    int n_lines = 0, cap_lines = 0;

    FILE *f = fopen(path, "r");
    if (f) {
        char buf[512];
        while (fgets(buf, sizeof(buf), f)) {
            char *copy = strdup(buf);
            if (!copy) break;
            if (n_lines == cap_lines) {
                int nc = cap_lines ? cap_lines * 2 : 16;
                char **nl = realloc(lines, (size_t)nc * sizeof(char *));
                if (!nl) { free(copy); break; }
                lines = nl;
                cap_lines = nc;
            }
            lines[n_lines++] = copy;
        }
        fclose(f);
    }

    int w = 0;
    for (int i = 0; i < n_lines; i++) {
        char *p = lines[i];
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "theme", 5) == 0) {
            char *q = p + 5;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') { free(lines[i]); continue; }
        }
        lines[w++] = lines[i];
    }
    n_lines = w;

    char newline[128];
    snprintf(newline, sizeof(newline), "theme = %s\n",
             THEMES[g_current].name);
    char *copy = strdup(newline);
    if (copy) {
        if (n_lines == cap_lines) {
            int nc = cap_lines ? cap_lines * 2 : 16;
            char **nl = realloc(lines, (size_t)nc * sizeof(char *));
            if (nl) { lines = nl; cap_lines = nc; }
        }
        if (n_lines < cap_lines) {
            lines[n_lines++] = copy;
        } else {
            free(copy);
        }
    }

    f = fopen(path, "w");
    if (!f) {
        for (int i = 0; i < n_lines; i++) free(lines[i]);
        free(lines);
        return;
    }
    for (int i = 0; i < n_lines; i++) {
        fputs(lines[i], f);
        free(lines[i]);
    }
    fclose(f);
    free(lines);
}