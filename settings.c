// language: C, file: wisp-code/settings.c
#ifdef _WIN32
    #include "posix.h"
#endif

#include "settings.h"
#include "tui.h"
#include "skin.h"
#include "console.h"     /* for http_get_text, g_project, wisp_logf */

#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

/* ================================================================== */
/*  Row model                                                         */
/*                                                                    */
/*  Each row is one of three kinds:
 *    ROW_TEXT     — free-form editable text (or int if is_int is set)
 *    ROW_CYCLE    — a small inline list; ←/→ cycles, Enter does nothing
 *    ROW_READONLY — displayed but skipped by navigation and editing
 *
 *  The endpoint row is a cycler with two values. The model row
 *  switches between editable text and a cycler depending on the
 *  current endpoint — when the endpoint is OpenRouter and the free
 *  model list has been fetched, it becomes a cycler over that list.
 *
 *  timeout_s stores seconds (a value a human can read at a glance);
 *  settings_timeout_ms() converts to milliseconds for the HTTP layer.
 *
 *  rules_md and operator_md name Markdown files at the project root.
 *  Their contents are loaded once at startup by console.c and
 *  appended to the system prefix inside history[0]. They are not read
 *  by this module.
 * ================================================================== */

typedef enum {
    ROW_TEXT = 0,
    ROW_CYCLE,
    ROW_READONLY,
} row_kind_t;

typedef struct {
    const char *key;        /* "api_key"                             */
    const char *label;      /* "api_key" — same for now              */
    char        value[256]; /* current value (editable)              */
    int         masked;     /* 1 = render as sk-or-…abc              */
    int         is_int;     /* 1 = numeric only on edit              */
    row_kind_t  kind;       /* ROW_TEXT / ROW_CYCLE / ROW_READONLY   */
} settings_row_t;

/* Endpoint values. Two strings, cycled in place. */
static const char *ENDPOINT_VALUES[] = { "openrouter", "custom" };
#define ENDPOINT_COUNT 2

/* OpenRouter model list endpoint. */
#define OPENROUTER_MODELS_URL "https://openrouter.ai/api/v1/models"

/* The compiled-in URL. When endpoint is OpenRouter, api_url is forced
 * to this and locked. When endpoint is custom, the user can edit it. */
#define OPENROUTER_URL "https://openrouter.ai/api/v1/chat/completions"

/* Free-model list, populated on demand by F. */
#define MODEL_LIST_MAX 256
static char      g_model_list[MODEL_LIST_MAX][128];
static int       g_model_count = 0;
static int       g_models_fetched = 0;
static char      g_models_error[128] = {0};

/* Row indices, so the handlers can refer to rows by name. */
enum {
    ROW_IDX_ENDPOINT = 0,
    ROW_IDX_API_URL,
    ROW_IDX_API_KEY,
    ROW_IDX_MODEL,
    ROW_IDX_RULES,
    ROW_IDX_OPERATOR,
    ROW_IDX_OSC8,
    ROW_IDX_TIMEOUT,
};

#define ROW_COUNT 8

static settings_row_t g_rows[ROW_COUNT] = {
    { "endpoint",    "endpoint",    "openrouter", 0, 0, ROW_CYCLE    },
    { "api_url",     "api_url",     OPENROUTER_URL, 0, 0, ROW_READONLY },
    { "api_key",     "api_key",     "", 1, 0, ROW_TEXT },
    { "model",       "model",       "", 0, 0, ROW_TEXT },
    { "rules_md",    "rules_md",    "", 0, 0, ROW_TEXT },
    { "operator_md", "operator_md", "", 0, 0, ROW_TEXT },
    { "osc8",        "osc8",        "auto", 0, 0, ROW_CYCLE    },
    { "timeout_s",   "timeout_s",   "180", 0, 1, ROW_TEXT     },
};

/* ================================================================== */
/*  Overlay state                                                     */
/* ================================================================== */

static int  g_open        = 0;
static int  g_sel         = 0;
static int  g_editing     = 0;
static char g_edit_buf[256];
static int  g_edit_len    = 0;
static int  g_edit_cursor = 0;


/* Last successful-or-attempted fetch, in monotonic ms. Guards against
 * a user mashing F and hammering the upstream endpoint. */
static long long g_last_fetch_ms = 0;
#define FETCH_COOLDOWN_MS 5000

/* ================================================================== */
/*  Local monotonic clock                                             */
/* ================================================================== */

static long long settings_monotonic_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000LL + t.tv_nsec / 1000000LL;
}

/* ================================================================== */
/*  Public open/close/is_open                                         */
/* ================================================================== */

void settings_open(void) {
    g_open = 1;
    g_sel = 0;
    g_editing = 0;
    g_edit_len = 0;
    g_edit_cursor = 0;
}

void settings_close(void) {
    g_open = 0;
    g_editing = 0;
}

int settings_is_open(void) {
    return g_open;
}

int settings_is_editing(void) {
    return g_open && g_editing;
}

/* ================================================================== */
/*  Endpoint / row kind helpers                                       */
/* ================================================================== */

static int endpoint_is_openrouter(void) {
    return strcmp(g_rows[ROW_IDX_ENDPOINT].value, "openrouter") == 0;
}

/* The effective kind of a row. Most rows have a fixed kind; the model
 * row switches between ROW_TEXT and ROW_CYCLE depending on whether
 * the endpoint is OpenRouter and the free list has been fetched. */
static row_kind_t effective_kind(int idx) {
    if (idx == ROW_IDX_MODEL) {
        if (endpoint_is_openrouter() && g_model_count > 0) return ROW_CYCLE;
        return ROW_TEXT;
    }
    return g_rows[idx].kind;
}

/* Number of values a ROW_CYCLE row can cycle through. */
static int cycle_count(int idx) {
    if (idx == ROW_IDX_ENDPOINT)   return ENDPOINT_COUNT;
    if (idx == ROW_IDX_OSC8)       return 3;   /* auto / on / off */
    if (idx == ROW_IDX_MODEL && endpoint_is_openrouter() && g_model_count > 0)
        return g_model_count;
    return 0;
}

/* Value at position `pos` in a ROW_CYCLE row. Writes into `out`. */
static void cycle_value_at(int idx, int pos, char *out, size_t out_sz) {
    if (idx == ROW_IDX_ENDPOINT) {
        if (pos < 0 || pos >= ENDPOINT_COUNT) pos = 0;
        snprintf(out, out_sz, "%s", ENDPOINT_VALUES[pos]);
        return;
    }
    if (idx == ROW_IDX_OSC8) {
        static const char *vals[] = { "auto", "on", "off" };
        if (pos < 0 || pos >= 3) pos = 0;
        snprintf(out, out_sz, "%s", vals[pos]);
        return;
    }
    if (idx == ROW_IDX_MODEL) {
        if (pos < 0 || pos >= g_model_count) pos = 0;
        snprintf(out, out_sz, "%s", g_model_list[pos]);
        return;
    }
    out[0] = '\0';
}

/* Current position of a ROW_CYCLE row's value in its value list.
 * Returns 0 if not found. */
static int cycle_position(int idx) {
    int n = cycle_count(idx);
    char buf[256];
    for (int i = 0; i < n; i++) {
        cycle_value_at(idx, i, buf, sizeof(buf));
        if (strcmp(buf, g_rows[idx].value) == 0) return i;
    }
    return 0;
}

/* Advance (dir=+1) or retreat (dir=-1) a ROW_CYCLE row. Wraps. */
static void cycle_step(int idx, int dir);

/* ================================================================== */
/*  Persistence                                                       */
/* ================================================================== */

static int conf_set_many(const char *root,
                         const char **keys,
                         const char **values,
                         int n) {
    if (!root || !root[0] || !keys || !values || n <= 0) return -1;

    char path[MAX_PATH_LEN * 2 + 32];
    snprintf(path, sizeof(path), "%s/wisp-code.conf", root);

    char tmp[MAX_PATH_LEN * 2 + 64];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, ( int )wisp_getpid());

    char **lines = NULL;
    int n_lines = 0, cap_lines = 0;

    FILE *f = fopen(path, "r");
    if (f) {
        char buf[4096];
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
        int is_ours = 0;
        for (int k = 0; k < n; k++) {
            size_t klen = strlen(keys[k]);
            if (strncmp(p, keys[k], klen) == 0) {
                char *q = p + klen;
                while (*q == ' ' || *q == '\t') q++;
                if (*q == '=') { is_ours = 1; break; }
            }
        }
        if (is_ours) { free(lines[i]); continue; }
        lines[w++] = lines[i];
    }
    n_lines = w;

    for (int k = 0; k < n; k++) {
        if (!values[k]) continue;
        char line[512];
        snprintf(line, sizeof(line), "%s = %s\n", keys[k], values[k]);
        char *copy = strdup(line);
        if (!copy) continue;
        if (n_lines == cap_lines) {
            int nc = cap_lines ? cap_lines * 2 : 16;
            char **nl = realloc(lines, (size_t)nc * sizeof(char *));
            if (!nl) { free(copy); break; }
            lines = nl;
            cap_lines = nc;
        }
        lines[n_lines++] = copy;
    }

    FILE *out = fopen(tmp, "w");
    if (!out) {
        for (int i = 0; i < n_lines; i++) free(lines[i]);
        free(lines);
        return -1;
    }
    for (int i = 0; i < n_lines; i++) {
        fputs(lines[i], out);
        free(lines[i]);
    }
    fclose(out);
    free(lines);

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

void settings_save(const char *project_root) {
    if (!project_root || !project_root[0]) return;

    const char *keys[ROW_COUNT];
    const char *vals[ROW_COUNT];
    int n = 0;

    for (int i = 0; i < ROW_COUNT; i++) {
        if (i == ROW_IDX_API_URL && endpoint_is_openrouter()) continue;
        keys[n] = g_rows[i].key;
        vals[n] = g_rows[i].value;
        n++;
    }

    if (conf_set_many(project_root, keys, vals, n) == 0) {
        wisp_logf("settings | saved %d key%s to wisp-code.conf",
                  n, n == 1 ? "" : "s");
    } else {
        wisp_logf("settings | save failed");
        ops_push("settings: save failed (see log)");
    }
}

void settings_load(const char *project_root) {
    if (!project_root || !project_root[0]) return;

    char buf[256];

    if (conf_get(project_root, "endpoint", buf, sizeof(buf)) == 0) {
        if (strcmp(buf, "openrouter") == 0 || strcmp(buf, "custom") == 0) {
            snprintf(g_rows[ROW_IDX_ENDPOINT].value,
                     sizeof(g_rows[ROW_IDX_ENDPOINT].value), "%s", buf);
        }
    }

    if (endpoint_is_openrouter()) {
        snprintf(g_rows[ROW_IDX_API_URL].value,
                 sizeof(g_rows[ROW_IDX_API_URL].value),
                 "%s", OPENROUTER_URL);
        g_rows[ROW_IDX_API_URL].kind = ROW_READONLY;
    } else {
        if (conf_get(project_root, "api_url", buf, sizeof(buf)) == 0) {
            snprintf(g_rows[ROW_IDX_API_URL].value,
                     sizeof(g_rows[ROW_IDX_API_URL].value), "%s", buf);
        }
        g_rows[ROW_IDX_API_URL].kind = ROW_TEXT;
    }

    if (conf_get(project_root, "api_key", buf, sizeof(buf)) == 0) {
        snprintf(g_rows[ROW_IDX_API_KEY].value,
                 sizeof(g_rows[ROW_IDX_API_KEY].value), "%s", buf);
    }

    if (conf_get(project_root, "model", buf, sizeof(buf)) == 0) {
        snprintf(g_rows[ROW_IDX_MODEL].value,
                 sizeof(g_rows[ROW_IDX_MODEL].value), "%s", buf);
    }

    if (conf_get(project_root, "rules_md", buf, sizeof(buf)) == 0) {
        snprintf(g_rows[ROW_IDX_RULES].value,
                 sizeof(g_rows[ROW_IDX_RULES].value), "%s", buf);
    }

    if (conf_get(project_root, "operator_md", buf, sizeof(buf)) == 0) {
        snprintf(g_rows[ROW_IDX_OPERATOR].value,
                 sizeof(g_rows[ROW_IDX_OPERATOR].value), "%s", buf);
    }

    if (conf_get(project_root, "osc8", buf, sizeof(buf)) == 0) {
        if (strcmp(buf, "auto") == 0 ||
            strcmp(buf, "on") == 0 ||
            strcmp(buf, "off") == 0) {
            snprintf(g_rows[ROW_IDX_OSC8].value,
                     sizeof(g_rows[ROW_IDX_OSC8].value), "%s", buf);
        }
    }

    if (conf_get(project_root, "timeout_s", buf, sizeof(buf)) == 0) {
        int ok = buf[0] != '\0';
        for (const char *p = buf; *p; p++) {
            if (*p < '0' || *p > '9') { ok = 0; break; }
        }
        if (ok) {
            snprintf(g_rows[ROW_IDX_TIMEOUT].value,
                     sizeof(g_rows[ROW_IDX_TIMEOUT].value), "%s", buf);
        }
    }

    wisp_logf("settings | loaded (endpoint=%s)",
              g_rows[ROW_IDX_ENDPOINT].value);
}

/* ================================================================== */
/*  Text helpers                                                      */
/* ================================================================== */

static int disp_width(const char *s) {
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if ((*p & 0xC0) != 0x80) w++;
        p++;
    }
    return w;
}

static void format_value(const settings_row_t *r, char *out, size_t out_sz) {
    if (!r->masked) {
        snprintf(out, out_sz, "%s", r->value);
        return;
    }
    size_t n = strlen(r->value);
    if (n <= 12) {
        snprintf(out, out_sz, "%s", r->value);
        return;
    }
    snprintf(out, out_sz, "%.8s…%s", r->value, r->value + n - 3);
}

static void fit_value(const char *value, int max, char *out, size_t out_sz) {
    if (max < 4) max = 4;
    int w = disp_width(value);
    if (w <= max) {
        snprintf(out, out_sz, "%s", value);
        return;
    }
    int acc = 0;
    const unsigned char *p = (const unsigned char *)value;
    while (*p && acc < max - 1) {
        int adv;
        if      ((*p & 0x80) == 0x00) adv = 1;
        else if ((*p & 0xE0) == 0xC0) adv = 2;
        else if ((*p & 0xF0) == 0xE0) adv = 3;
        else if ((*p & 0xF8) == 0xF0) adv = 4;
        else                          adv = 1;
        p += adv;
        acc++;
    }
    size_t n = (size_t)((const char *)p - value);
    if (n >= out_sz - 3) n = out_sz - 4;
    memcpy(out, value, n);
    out[n] = '\0';
    strncat(out, "…", out_sz - strlen(out) - 1);
}

/* ================================================================== */
/*  Layout                                                            */
/* ================================================================== */

typedef struct {
    int row;
    int col;
    int width;
    int height;
} box_t;

static void compute_box(box_t *b) {
    int rows = tui_rows();
    int cols = tui_cols();

    b->height = ROW_COUNT + 6;
    if (b->height > rows - 2) b->height = rows - 2;

    b->width = 74;
    if (b->width > (cols * 9) / 10) b->width = (cols * 9) / 10;
    if (b->width < 40) b->width = 40;

    b->row = (rows - b->height) / 2;
    if (b->row < 1) b->row = 1;
    b->col = (cols - b->width) / 2;
    if (b->col < 1) b->col = 1;
}

/* ================================================================== */
/*  Render                                                            */
/* ================================================================== */

static void write_str(const char *s) {
    if (!s || !s[0]) return;
    ssize_t w = write(STDOUT_FILENO, s, (unsigned int)strlen(s));
    (void)w;
}

static void draw_box_border(const box_t *b) {
    tui_goto(b->row, b->col);
    write_str(C_BORDER);
    write_str(BX_TL);

    const char *title = " Settings ";
    write_str(BX_H);
    write_str(BX_H);
    write_str(C_RESET);
    write_str(C_TITLE);
    write_str(C_BOLD);
    write_str(title);
    write_str(C_RESET);

    int used = 2 + disp_width(title);
    write_str(C_BORDER);
    while (used < b->width - 2) {
        write_str(BX_H);
        used++;
    }
    write_str(BX_TR);
    write_str(C_RESET);

    tui_goto(b->row + b->height - 1, b->col);
    write_str(C_BORDER);
    write_str(BX_BL);
    for (int i = 0; i < b->width - 2; i++) write_str(BX_H);
    write_str(BX_BR);
    write_str(C_RESET);

    for (int r = 1; r < b->height - 1; r++) {
        tui_goto(b->row + r, b->col);
        write_str(C_BORDER);
        write_str(BX_V);
        write_str(C_RESET);

        tui_goto(b->row + r, b->col + b->width - 1);
        write_str(C_BORDER);
        write_str(BX_V);
        write_str(C_RESET);
    }
}

static void clear_box_interior(const box_t *b) {
    char spaces[128];
    int span = b->width - 2;
    while (span > 0) {
        int chunk = span > (int)sizeof(spaces) ? (int)sizeof(spaces) : span;
        memset(spaces, ' ', chunk);
        for (int r = 1; r < b->height - 1; r++) {
            tui_goto(b->row + r, b->col + 1);
            { ssize_t w = write(STDOUT_FILENO, spaces, chunk); (void)w; }
        }
        span -= chunk;
    }
}

static void display_value(int idx, char *out, size_t out_sz) {
    const settings_row_t *r = &g_rows[idx];

    if (idx == ROW_IDX_MODEL && endpoint_is_openrouter()
        && g_models_fetched && g_model_count == 0 && !g_models_error[0]) {
        snprintf(out, out_sz, "(no free models found)");
        return;
    }
    if (idx == ROW_IDX_MODEL && endpoint_is_openrouter() && g_models_error[0]) {
        snprintf(out, out_sz, "(fetch failed: %s)", g_models_error);
        return;
    }
    if ((idx == ROW_IDX_RULES || idx == ROW_IDX_OPERATOR)
        && r->value[0] == '\0') {
        snprintf(out, out_sz, "(none)");
        return;
    }

    char masked[300];
    format_value(r, masked, sizeof(masked));
    snprintf(out, out_sz, "%s", masked);
}

static void draw_row(const box_t *b, int idx, int screen_row, int selected) {
    const settings_row_t *r = &g_rows[idx];

    int inner_left  = b->col + 2;
    int inner_right = b->col + b->width - 2;

    const int label_w = 12;
    int value_col = inner_left + label_w + 2;
    int value_w   = inner_right - value_col;
    if (value_w < 4) value_w = 4;
    if (value_w <= 0) return;

    char value_buf[300];
    display_value(idx, value_buf, sizeof(value_buf));

    char fitted[300];
    fit_value(value_buf, value_w, fitted, sizeof(fitted));

    if (selected) {
        write_str("\x1b[7m\x1b[1m");
    } else if (effective_kind(idx) == ROW_READONLY) {
        write_str(C_MUTED);
    } else {
        write_str(C_INFO);
    }

    char line[512];
    int n = snprintf(line, sizeof(line), " %-*s  %s", label_w, r->label, fitted);
    if (n < 0) return;

    tui_puts_at_padded(screen_row, b->col + 1, b->col + b->width - 2, line);
    write_str(C_RESET);
}

/* Paint the value region of the row currently being edited. Cursor
 * placement is NOT done here — see place_editing_cursor(), called
 * last from settings_render(), because any tui_goto issued after
 * this function (in particular the one at the end of
 * draw_hint_line) would move the terminal cursor off the edit
 * position. */
static void draw_editing_row(const box_t *b, int idx, int screen_row) {
    const settings_row_t *r = &g_rows[idx];

    int inner_left  = b->col + 2;
    int inner_right = b->col + b->width - 2;

    const int label_w = 12;
    int value_col = inner_left + label_w + 2;
    int value_w   = inner_right - value_col;
    if (value_w < 4) value_w = 4;

    char label[16];
    snprintf(label, sizeof(label), "%-*s", label_w, r->label);
    write_str(C_INFO);
    tui_puts_at_padded(screen_row, b->col + 1, value_col, label);
    write_str(C_RESET);

    int disp = value_w - 1;
    if (disp < 1) disp = 1;

    /* Horizontal scroll: shift the window when the cursor runs past
     * the visible width. `start` is the byte offset of the first
     * character shown. */
    int start = 0;
    if (g_edit_cursor > disp) start = g_edit_cursor - disp;

    char visible[300];
    int n = g_edit_len - start;
    if (n > disp) n = disp;
    if (n < 0) n = 0;
    memcpy(visible, g_edit_buf + start, n);
    visible[n] = '\0';

    /* Paint the value region and pad so any bytes from the previous
     * render are cleared. */
    write_str(C_ACCENT);
    tui_goto(screen_row, value_col);
    { ssize_t w = write(STDOUT_FILENO, visible, (size_t)n); (void)w; }
    for (int c = n; c < disp; c++) write_str(" ");
    write_str(C_RESET);
}

/* Move the terminal cursor to the current edit position. Does NOT
 * show the cursor — render_frame() issues the single \x1b[?25h for
 * the frame, after every paint call has finished. Must be called
 * last from settings_render(), because the tui_goto calls in
 * draw_hint_line() would otherwise leave the cursor on the hint row. */
static void place_editing_cursor(const box_t *b) {
    if (!g_editing) return;

    int inner_left  = b->col + 2;
    int inner_right = b->col + b->width - 2;

    const int label_w = 12;
    int value_col = inner_left + label_w + 2;
    int value_w   = inner_right - value_col;
    if (value_w < 4) value_w = 4;
    int disp = value_w - 1;
    if (disp < 1) disp = 1;

    int start = 0;
    if (g_edit_cursor > disp) start = g_edit_cursor - disp;

    int screen_row = b->row + 2 + g_sel;

    int cursor_col = value_col + (g_edit_cursor - start);
    if (cursor_col > inner_right) cursor_col = inner_right;
    if (cursor_col < value_col)   cursor_col = value_col;

    tui_goto(screen_row, cursor_col);
}

static void draw_hint_line(const box_t *b, int screen_row) {
    const char *hint;
    if (g_editing) {
        hint = " Enter · Commit   Esc · Cancel   ^U · Clear ";
    } else if (g_sel == ROW_IDX_MODEL && endpoint_is_openrouter()) {
        hint = " ↑↓ · Move   ←→ · Cycle   F · Fetch   Tab · Next   Esc · Close ";
    } else if (g_sel == ROW_IDX_TIMEOUT) {
        hint = " ↑↓ · Move   Enter · Edit   Value in seconds   Esc · Close ";
    } else if (g_sel == ROW_IDX_RULES) {
        hint = " ↑↓ · Move   Enter · Edit   Filename (reload) or empty   Esc · Close ";
    } else if (effective_kind(g_sel) == ROW_CYCLE) {
        hint = " ↑↓ · Move   ←→ · Cycle   Tab · Next   Esc · Close ";
    } else {
        hint = " ↑↓ · Move   Tab · Next   Enter · Edit   Esc · Close ";
    }

    int inner_left  = b->col + 1;
    int inner_right = b->col + b->width - 2;   /* one left of the border */
    int avail = inner_right - inner_left + 1;
    if (avail < 1) return;

    /* Clip the hint to `avail` display columns, walking to a UTF-8
     * boundary so we never split a multi-byte glyph. */
    const char *p   = hint;
    const char *end = hint + strlen(hint);
    int w = 0;
    while (p < end && w < avail) {
        unsigned char c = (unsigned char)*p;
        int adv;
        if      ((c & 0x80) == 0x00) adv = 1;
        else if ((c & 0xE0) == 0xC0) adv = 2;
        else if ((c & 0xF0) == 0xE0) adv = 3;
        else if ((c & 0xF8) == 0xF0) adv = 4;
        else                         adv = 1;
        p += adv;
        w += 1;
    }

    write_str(C_HINT);
    write_str(C_BOLD);

    /* Emit the clipped prefix, then pad with spaces to the inner
     * right edge so the row is fully painted and the color resets
     * on a clean boundary. */
    tui_goto(screen_row, inner_left);
    { ssize_t wr = write(STDOUT_FILENO, hint, (unsigned int)(p - hint)); (void)wr; }
    for (int c = w; c < avail; c++) write_str(" ");
    write_str(C_RESET);

    /* Repaint the right border, in case the write above touched it. */
    tui_goto(screen_row, b->col + b->width - 1);
    write_str(C_BORDER);
    write_str(BX_V);
    write_str(C_RESET);

    /* Repaint the left border too — symmetric, and cheap. */
    tui_goto(screen_row, b->col);
    write_str(C_BORDER);
    write_str(BX_V);
    write_str(C_RESET);
}

void settings_render(void) {
    if (!g_open) return;

    box_t b;
    compute_box(&b);

    /* The cursor is hidden for the whole frame by render_frame().
     * This function only paints and positions; the single
     * \x1b[?25h for the frame is issued by render_frame() after this
     * returns. Never write \x1b[?25l or \x1b[?25h here. */

    draw_box_border(&b);
    clear_box_interior(&b);

    int first_row = b.row + 2;
    for (int i = 0; i < ROW_COUNT; i++) {
        if (first_row + i >= b.row + b.height - 2) break;
        if (g_editing && i == g_sel) {
            draw_editing_row(&b, i, first_row + i);
        } else {
            draw_row(&b, i, first_row + i, i == g_sel);
        }
    }

    draw_hint_line(&b, b.row + b.height - 2);

    /* Cursor placement must be the very last thing we do. Any
     * tui_goto() after this point — including the one at the tail
     * of draw_hint_line() above — would move the terminal cursor
     * away from the edit position. */
    if (g_editing) {
        place_editing_cursor(&b);
    } else {
        /* Park the cursor somewhere harmless. It's hidden anyway. */
        tui_goto(b.row, b.col);
    }
}

/* ================================================================== */
/*  Fetch free models from OpenRouter                                 */
/* ================================================================== */

static void parse_free_models(const char *json) {
    g_model_count = 0;

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        snprintf(g_models_error, sizeof(g_models_error), "bad JSON");
        return;
    }
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsArray(data)) {
        cJSON_Delete(root);
        snprintf(g_models_error, sizeof(g_models_error), "no data array");
        return;
    }

    int n = cJSON_GetArraySize(data);
    for (int i = 0; i < n && g_model_count < MODEL_LIST_MAX; i++) {
        cJSON *m = cJSON_GetArrayItem(data, i);
        if (!cJSON_IsObject(m)) continue;

        cJSON *pricing = cJSON_GetObjectItem(m, "pricing");
        if (!cJSON_IsObject(pricing)) continue;

        cJSON *pp = cJSON_GetObjectItem(pricing, "prompt");
        cJSON *pc = cJSON_GetObjectItem(pricing, "completion");
        if (!cJSON_IsString(pp) || !cJSON_IsString(pc)) continue;
        if (strcmp(pp->valuestring, "0") != 0) continue;
        if (strcmp(pc->valuestring, "0") != 0) continue;

        cJSON *id = cJSON_GetObjectItem(m, "id");
        if (!cJSON_IsString(id)) continue;
        if (strlen(id->valuestring) >= sizeof(g_model_list[0])) continue;

        /* The "openrouter/free" router picks a random free model per
        * request. Consecutive turns land on different providers, which
        * defeats the prefix cache and can silently switch the context
        * window between turns. Never offer it as a choice. */
        if (strcmp(id->valuestring, "openrouter/free") == 0) continue;

        snprintf(g_model_list[g_model_count],
                sizeof(g_model_list[0]),
                "%s", id->valuestring);

        g_model_count++;
    }

    cJSON_Delete(root);
}

static void fetch_models(void) {
    long long now = settings_monotonic_ms();
    if (g_last_fetch_ms != 0 && now - g_last_fetch_ms < FETCH_COOLDOWN_MS) {
        wisp_logf("settings | fetch ignored (cooldown, %lldms since last)",
                  now - g_last_fetch_ms);
        return;
    }
    g_last_fetch_ms = now;

    g_models_fetched = 0;
    g_models_error[0] = '\0';
    g_model_count = 0;

    char err[128] = {0};
    char *body = http_get_text(OPENROUTER_MODELS_URL,
                               g_rows[ROW_IDX_API_KEY].value,
                               err, sizeof(err));
    if (!body) {
        snprintf(g_models_error, sizeof(g_models_error),
                 "%s", err[0] ? err : "request failed");
        g_models_fetched = 1;
        return;
    }

    parse_free_models(body);
    free(body);

    g_models_fetched = 1;

    if (g_models_error[0]) return;

    int found = 0;
    for (int i = 0; i < g_model_count; i++) {
        if (strcmp(g_model_list[i], g_rows[ROW_IDX_MODEL].value) == 0) {
            found = 1;
            break;
        }
    }
    if (!found && g_model_count > 0) {
        snprintf(g_rows[ROW_IDX_MODEL].value,
                 sizeof(g_rows[ROW_IDX_MODEL].value),
                 "%s", g_model_list[0]);
    }

    settings_save(g_project.root);
}

/* ================================================================== */
/*  Key handling                                                      */
/* ================================================================== */

static void begin_edit(void) {
    const settings_row_t *r = &g_rows[g_sel];
    snprintf(g_edit_buf, sizeof(g_edit_buf), "%s", r->value);
    g_edit_len = (int)strlen(g_edit_buf);
    g_edit_cursor = g_edit_len;
    g_editing = 1;
}

static void commit_edit(void) {
    g_edit_buf[g_edit_len] = '\0';
    settings_row_t *r = &g_rows[g_sel];
    snprintf(r->value, sizeof(r->value), "%s", g_edit_buf);
    g_editing = 0;
    settings_save(g_project.root);

    /* The rules and operator files are read once at startup. Editing
     * the setting mid-session has no effect until the next launch.
     * Tell the user so they don't expect the change to take effect
     * immediately. */
    if (g_sel == ROW_IDX_RULES) {
        tr_push_system("[rules_md changed — restart wisp-code to apply]");
    } else if (g_sel == ROW_IDX_OPERATOR) {
        tr_push_system("[operator_md changed — restart wisp-code to apply]");
    }
}

static void cancel_edit(void) {
    g_editing = 0;
    g_edit_len = 0;
    g_edit_cursor = 0;
}

static int is_allowed_char(const settings_row_t *r, unsigned char c) {
    if (c < 0x20 || c == 0x7f) return 0;
    if (r->is_int) return (c >= '0' && c <= '9');
    return 1;
}

static int handle_editing_key(int ch) {
    settings_row_t *r = &g_rows[g_sel];

    switch (ch) {
        case 13:                        /* Enter — commit */
            commit_edit();
            return 1;

        case 27:                        /* Esc — cancel (handled by caller) */
            return 0;

        case 127:                       /* Backspace */
        case 8:
            if (g_edit_cursor > 0) {
                memmove(g_edit_buf + g_edit_cursor - 1,
                        g_edit_buf + g_edit_cursor,
                        (size_t)(g_edit_len - g_edit_cursor + 1));
                g_edit_cursor--;
                g_edit_len--;
            }
            return 1;

        case 21:                        /* Ctrl-U — clear */
            g_edit_len = 0;
            g_edit_cursor = 0;
            g_edit_buf[0] = '\0';
            return 1;

        case 1:                         /* Ctrl-A — home */
            g_edit_cursor = 0;
            return 1;

        case 5:                         /* Ctrl-E — end */
            g_edit_cursor = g_edit_len;
            return 1;

        case 4:                         /* Ctrl-D — delete forward */
            if (g_edit_cursor < g_edit_len) {
                memmove(g_edit_buf + g_edit_cursor,
                        g_edit_buf + g_edit_cursor + 1,
                        (size_t)(g_edit_len - g_edit_cursor));
                g_edit_len--;
            }
            return 1;

        default:
            break;
    }

    if (ch >= 0x20 && is_allowed_char(r, (unsigned char)ch)
        && g_edit_len < (int)sizeof(g_edit_buf) - 1) {
        memmove(g_edit_buf + g_edit_cursor + 1,
                g_edit_buf + g_edit_cursor,
                (size_t)(g_edit_len - g_edit_cursor + 1));
        g_edit_buf[g_edit_cursor] = (char)ch;
        g_edit_cursor++;
        g_edit_len++;
        return 1;
    }

    return 0;
}

static int handle_navigation_key(int ch) {
    switch (ch) {
        case 13:
            if (effective_kind(g_sel) == ROW_TEXT) {
                begin_edit();
                return 1;
            }
            return 0;

        case 'j':
        case 14:
            if (g_sel + 1 < ROW_COUNT) g_sel++;
            return 1;

        case 'k':
        case 16:
            if (g_sel > 0) g_sel--;
            return 1;

        case 9:
            g_sel = (g_sel + 1) % ROW_COUNT;
            return 1;

        case 'F':
        case 'f':
            if (g_sel == ROW_IDX_MODEL && endpoint_is_openrouter()) {
                fetch_models();
                return 1;
            }
            return 0;

        default:
            return 0;
    }
}

static void cycle_step(int idx, int dir) {
    int n = cycle_count(idx);
    if (n <= 0) return;
    int pos = cycle_position(idx);
    pos = (pos + dir + n) % n;
    char buf[256];
    cycle_value_at(idx, pos, buf, sizeof(buf));
    snprintf(g_rows[idx].value, sizeof(g_rows[idx].value), "%s", buf);

    if (idx == ROW_IDX_ENDPOINT) {
        if (endpoint_is_openrouter()) {
            snprintf(g_rows[ROW_IDX_API_URL].value,
                     sizeof(g_rows[ROW_IDX_API_URL].value),
                     "%s", OPENROUTER_URL);
            g_rows[ROW_IDX_API_URL].kind = ROW_READONLY;
        } else {
            g_rows[ROW_IDX_API_URL].kind = ROW_TEXT;
        }
    }

    settings_save(g_project.root);
}

/* Return values:
 *   -1  = lone ESC (cancel/close)
 *    0  = unrecognized
 *   'A' = up, 'B' = down, 'C' = right, 'D' = left
 *   1   = Home, 2 = End, 3 = Delete, 4 = PgUp, 5 = PgDn
 */
static int read_esc_sequence(void) {
    int a = -1;
    for (int i = 0; i < 3 && a < 0; i++) {
        a = tui_try_getch();
        if (a < 0) usleep(1000);
    }
    if (a < 0) return -1;
    if (a != '[' && a != 'O') return 0;

    int b = -1;
    for (int i = 0; i < 3 && b < 0; i++) {
        b = tui_try_getch();
        if (b < 0) usleep(1000);
    }
    if (b < 0) return 0;

    if (b == 'A' || b == 'B' || b == 'C' || b == 'D') return b;
    if (b == 'H') return 1;    /* Home */
    if (b == 'F') return 2;    /* End */

    /* Numeric forms: ESC [ 1 ~ Home, 3 ~ Delete, 4 ~ End, 5 ~ PgUp, 6 ~ PgDn */
    int num = 0;
    int seen_tilde = 0;
    for (int i = 0; i < 4; i++) {
        int c = tui_try_getch();
        if (c < 0) break;
        if (c == '~') { seen_tilde = 1; break; }
        if (c >= '0' && c <= '9') {
            num = num * 10 + (c - '0');
        } else {
            return 0;
        }
    }
    if (!seen_tilde) return 0;

    switch (num) {
        case 1: return 1;   /* Home */
        case 3: return 3;   /* Delete */
        case 4: return 2;   /* End */
        case 5: return 4;   /* PgUp */
        case 6: return 5;   /* PgDn */
        default: return 0;
    }
}

int settings_handle_key(int ch) {
    if (!g_open) return 0;

    if (ch == 27) {
        int seq = read_esc_sequence();

        if (seq == -1) {
            if (g_editing) cancel_edit();
            else           settings_close();
            return 1;
        }
        if (seq == 0) return 1;

        if (g_editing) {
            if (seq == 'D') { if (g_edit_cursor > 0) g_edit_cursor--; return 1; }
            if (seq == 'C') { if (g_edit_cursor < g_edit_len) g_edit_cursor++; return 1; }
            if (seq == 1)   { g_edit_cursor = 0; return 1; }               /* Home */
            if (seq == 2)   { g_edit_cursor = g_edit_len; return 1; }      /* End */
            if (seq == 3) {                                                 /* Delete */
                if (g_edit_cursor < g_edit_len) {
                    memmove(g_edit_buf + g_edit_cursor,
                            g_edit_buf + g_edit_cursor + 1,
                            (size_t)(g_edit_len - g_edit_cursor));
                    g_edit_len--;
                }
                return 1;
            }
            if (seq == 'A' || seq == 'B') return 1;   /* swallow up/down */
            return 1;
        }

        if (seq == 'A') { if (g_sel > 0) g_sel--; return 1; }
        if (seq == 'B') { if (g_sel + 1 < ROW_COUNT) g_sel++; return 1; }
        if (seq == 'D') {
            if (effective_kind(g_sel) == ROW_CYCLE) cycle_step(g_sel, -1);
            return 1;
        }
        if (seq == 'C') {
            if (effective_kind(g_sel) == ROW_CYCLE) cycle_step(g_sel, +1);
            return 1;
        }
        return 1;
    }

    if (g_editing) return handle_editing_key(ch);
    return handle_navigation_key(ch);
}


/* ================================================================== */
/*  Effective runtime values                                          */
/* ================================================================== */

const char *settings_api_url(void) {
    return g_rows[ROW_IDX_API_URL].value;
}

const char *settings_api_key(void) {
    return g_rows[ROW_IDX_API_KEY].value;
}

const char *settings_model(void) {
    return g_rows[ROW_IDX_MODEL].value;
}

const char *settings_rules_file(void) {
    return g_rows[ROW_IDX_RULES].value;
}

const char *settings_operator_file(void) {
    return g_rows[ROW_IDX_OPERATOR].value;
}

int settings_timeout_ms(void) {
    const char *s = g_rows[ROW_IDX_TIMEOUT].value;
    if (!s || !s[0]) return 180000;

    long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return 180000;
        v = v * 10 + (*p - '0');
        if (v > 3600) return 180000;
    }
    if (v <= 0) return 180000;
    return (int)(v * 1000);
}

int settings_osc8_mode(void) {
    const char *s = g_rows[ROW_IDX_OSC8].value;
    if (!s || !s[0])          return 0;
    if (strcmp(s, "on") == 0) return 1;
    if (strcmp(s, "off") == 0) return 2;
    return 0;
}