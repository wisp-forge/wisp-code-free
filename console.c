// language: C, file: wisp-code/console.c
/*
 * console.c — wisp-code
 *
 * Layout (single column):
 *
 *   ● connected │ ws wisp/relay │ ctx 0B / 128k │ turn 0      errs 0 │ mode code │ [ember]
 *   ─────────────────────────────────────────────────────────────────────────────
 *   > the name of the youtuber is DDgaminG...
 *   ┃ [+] [DONE]    read index.html      52 K
 *   ┃ [+] [DONE]    write index.html      4 sec
 *   ┃ [MSG] Ready. Tell me what you want changed.
 *   ┃ [+] [MSG]    assistant                    3/14
 *   ┃  └─ long reply...
 *   > add a Minecraft one block section
 *   ┃ [+] [PENDING] task list created   4 tasks
 *   ┃ [FAILED]  write index.html
 *     ● waiting on upstream   esc to cancel
 *   ready.
 *   ─────────────────────────────────────────────────────────────────────────────
 *    Alt-B · Run ─ Alt-T · Theme ─ Alt-S · Settings ─ Tab · Focus ─ ^C · Quit
 */

#include "console.h"
#include "tui.h"
#include "skin.h"
#include "theme.h"
#include "platform.h"
#include "prompt.h"
#include "settings.h"
#include "highlight.h"
#include "port.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <signal.h>

#if defined(_WIN32)
  #include <io.h>
  #include <process.h>
  #include <direct.h>
  #include <windows.h>
  #include <sys/stat.h>
  /* POSIX shims for the functions console.c calls directly. */
  #ifndef STDOUT_FILENO
    #define STDOUT_FILENO 1
  #endif
  #ifndef STDIN_FILENO
    #define STDIN_FILENO 0
  #endif
#else
  #include <unistd.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <fcntl.h>
#endif

volatile int g_quit            = 0;
int          g_last_error      = 0;
int          g_shell_mode      = 0;

char  g_build_run[1024]        = {0};
char *g_pending_run_output     = NULL;

int g_body_visible = 20;
int g_last_reply_bytes = 0;

static int g_cap_in_focus = 0;

#define BUSY_HINT_AFTER_MS 5000

#define REPLY_OK          0
#define REPLY_READ_NEXT   1


/* ================================================================== */
/*  Monotonic clock                                                   */
/* ================================================================== */

static long long monotonic_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000LL + t.tv_nsec / 1000000LL;
}

/* ================================================================== */
/*  Per-run log                                                       */
/* ================================================================== */

static long long      g_log_start = 0;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

#if defined(_WIN32)
  #include <io.h>
  static int g_log_fd = -1;
  #define LOG_WRITE(fd, buf, n)  _write((fd), (buf), (unsigned)(n))
  #define LOG_OPEN(path)         _open((path), _O_WRONLY | _O_CREAT | _O_TRUNC, _S_IREAD | _S_IWRITE)
  #define LOG_CLOSE(fd)          _close(fd)
#else
  static int g_log_fd = -1;
  #define LOG_WRITE(fd, buf, n)  write((fd), (buf), (n))
  #define LOG_OPEN(path)         open((path), O_WRONLY | O_CREAT | O_TRUNC, 0644)
  #define LOG_CLOSE(fd)          close(fd)
#endif

void wisp_log_open(const char *project_root) {
    char path[MAX_PATH_LEN * 2 + 64];
    snprintf(path, sizeof(path), "%s/wisp-code-%d.log",
             project_root, (int)wisp_getpid());

    pthread_mutex_lock(&g_log_mutex);
    if (g_log_fd >= 0) { LOG_CLOSE(g_log_fd); g_log_fd = -1; }
    g_log_fd = LOG_OPEN(path);
    g_log_start = monotonic_ms();
    if (g_log_fd >= 0) {
        char hdr[256];
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        char stamp[64];
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
        int n = snprintf(hdr, sizeof(hdr),
                         "# wisp-code log — session %d\n# started %s\n# root: %s\n\n",
                         (int)wisp_getpid(), stamp, project_root);
        LOG_WRITE(g_log_fd, hdr, (size_t)n);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

void wisp_log_raw(const void *buf, size_t len) {
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_fd >= 0 && buf && len > 0) {
        LOG_WRITE(g_log_fd, buf, len);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

void wisp_logf(const char *fmt, ...) {
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_fd < 0) { pthread_mutex_unlock(&g_log_mutex); return; }

    long long ms = monotonic_ms() - g_log_start;
    long long sec = ms / 1000;
    long long frac = ms % 1000;
    char line[8192];
    int n = snprintf(line, sizeof(line), "[%5lld.%03lld] ", sec, frac);

    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);

    if (n < (int)sizeof(line) - 1) line[n++] = '\n';

    LOG_WRITE(g_log_fd, line, (size_t)n);
    pthread_mutex_unlock(&g_log_mutex);
}

void wisp_log_close(void) {
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_fd >= 0) {
        char tail[64];
        int n = snprintf(tail, sizeof(tail), "\n# log closed at %lldms\n",
                         monotonic_ms() - g_log_start);
        LOG_WRITE(g_log_fd, tail, (size_t)n);
        LOG_CLOSE(g_log_fd);
        g_log_fd = -1;
    }
    pthread_mutex_unlock(&g_log_mutex);
}

/* ================================================================== */
/*  Display width                                                     */
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

/* ================================================================== */
/*  Low-level output helpers                                          */
/* ================================================================== */

static void write_str(const char *s) {
    if (!s || !s[0]) return;
    ssize_t w = write(STDOUT_FILENO, s, ( unsigned int )strlen(s));
    (void)w;
}

static void emit_all(const char *first, ...) {
    va_list ap;
    va_start(ap, first);
    const char *s = first;
    while (s) {
        write_str(s);
        s = va_arg(ap, const char *);
    }
    va_end(ap);
}

/* ================================================================== */
/*  Conversation history                                              */
/* ================================================================== */
typedef struct {
    char  *role;
    char  *content;
} history_entry_t;

static history_entry_t g_history[HISTORY_MAX_ENTRIES];
static int             g_history_count = 0;
static pthread_mutex_t g_history_mutex = PTHREAD_MUTEX_INITIALIZER;

void history_init(const char *system_prefix) {
    pthread_mutex_lock(&g_history_mutex);
    for (int i = 0; i < g_history_count; i++) {
        free(g_history[i].role);
        free(g_history[i].content);
    }
    g_history_count = 0;
    if (system_prefix && system_prefix[0] && g_history_count < HISTORY_MAX_ENTRIES) {
        g_history[0].role    = strdup("system");
        g_history[0].content = strdup(system_prefix);
        if (g_history[0].role && g_history[0].content) {
            g_history_count = 1;
        }
    }
    pthread_mutex_unlock(&g_history_mutex);
}

void history_append_user(const char *content) {
    if (!content || !content[0]) return;
    pthread_mutex_lock(&g_history_mutex);
    if (g_history_count < HISTORY_MAX_ENTRIES) {
        g_history[g_history_count].role    = strdup("user");
        g_history[g_history_count].content = strdup(content);
        if (g_history[g_history_count].role && g_history[g_history_count].content) {
            g_history_count++;
        } else {
            free(g_history[g_history_count].role);
            free(g_history[g_history_count].content);
        }
    }
    pthread_mutex_unlock(&g_history_mutex);
}

void history_append_assistant(const char *content) {
    if (!content || !content[0]) return;
    pthread_mutex_lock(&g_history_mutex);
    if (g_history_count < HISTORY_MAX_ENTRIES) {
        g_history[g_history_count].role    = strdup("assistant");
        g_history[g_history_count].content = strdup(content);
        if (g_history[g_history_count].role && g_history[g_history_count].content) {
            g_history_count++;
        } else {
            free(g_history[g_history_count].role);
            free(g_history[g_history_count].content);
        }
    }
    pthread_mutex_unlock(&g_history_mutex);
}

void history_clear(void) {
    pthread_mutex_lock(&g_history_mutex);
    for (int i = 0; i < g_history_count; i++) {
        free(g_history[i].role);
        free(g_history[i].content);
    }
    g_history_count = 0;
    pthread_mutex_unlock(&g_history_mutex);
}

static void history_prune_locked(void) {
    if (g_history_count <= 1) return;

    size_t total = 0;
    for (int i = 0; i < g_history_count; i++) {
        if (g_history[i].content) total += strlen(g_history[i].content);
    }
    if (total <= HISTORY_MAX_BYTES) return;

    int drop_to = 1;
    while (drop_to < g_history_count && total > HISTORY_MAX_BYTES) {
        if (g_history[drop_to].content)
            total -= strlen(g_history[drop_to].content);
        drop_to++;
    }
    if ((drop_to - 1) % 2 != 0) drop_to++;

    for (int i = 1; i < drop_to; i++) {
        free(g_history[i].role);
        free(g_history[i].content);
    }
    int remaining = g_history_count - drop_to;
    for (int i = 0; i < remaining; i++) {
        g_history[1 + i] = g_history[drop_to + i];
    }
    g_history_count = 1 + remaining;
}

size_t history_bytes(void) {
    size_t total = 0;
    pthread_mutex_lock(&g_history_mutex);
    for (int i = 1; i < g_history_count; i++) {
        if (g_history[i].content) total += strlen(g_history[i].content);
    }
    pthread_mutex_unlock(&g_history_mutex);
    return total;
}

size_t history_system_len(void) {
    size_t n = 0;
    pthread_mutex_lock(&g_history_mutex);
    if (g_history_count > 0 && g_history[0].content) {
        n = strlen(g_history[0].content);
    }
    pthread_mutex_unlock(&g_history_mutex);
    return n;
}

size_t history_total_bytes(void) {
    return history_bytes() + history_system_len();
}

struct cJSON *history_to_json(void) {
    pthread_mutex_lock(&g_history_mutex);
    history_prune_locked();

    cJSON *arr = cJSON_CreateArray();
    if (!arr) { pthread_mutex_unlock(&g_history_mutex); return NULL; }

    for (int i = 0; i < g_history_count; i++) {
        cJSON *msg = cJSON_CreateObject();
        if (!msg) { cJSON_Delete(arr); pthread_mutex_unlock(&g_history_mutex); return NULL; }
        cJSON_AddStringToObject(msg, "role", g_history[i].role);
        cJSON_AddStringToObject(msg, "content", g_history[i].content);
        cJSON_AddItemToArray(arr, msg);
    }
    pthread_mutex_unlock(&g_history_mutex);
    return arr;
}

struct cJSON *history_to_json_pair(void) {
    pthread_mutex_lock(&g_history_mutex);
    history_prune_locked();

    cJSON *arr = cJSON_CreateArray();
    if (!arr) { pthread_mutex_unlock(&g_history_mutex); return NULL; }

    if (g_history_count < 1) {
        cJSON_Delete(arr);
        pthread_mutex_unlock(&g_history_mutex);
        return NULL;
    }
    {
        cJSON *msg = cJSON_CreateObject();
        if (!msg) { cJSON_Delete(arr); pthread_mutex_unlock(&g_history_mutex); return NULL; }
        cJSON_AddStringToObject(msg, "role", g_history[0].role);
        cJSON_AddStringToObject(msg, "content", g_history[0].content);
        cJSON_AddItemToArray(arr, msg);
    }
    if (g_history_count >= 2) {
        int last = g_history_count - 1;
        cJSON *msg = cJSON_CreateObject();
        if (!msg) { cJSON_Delete(arr); pthread_mutex_unlock(&g_history_mutex); return NULL; }
        cJSON_AddStringToObject(msg, "role", g_history[last].role);
        cJSON_AddStringToObject(msg, "content", g_history[last].content);
        cJSON_AddItemToArray(arr, msg);
    }
    pthread_mutex_unlock(&g_history_mutex);
    return arr;
}

struct cJSON *history_to_json_last(void) {
    pthread_mutex_lock(&g_history_mutex);
    history_prune_locked();

    cJSON *arr = cJSON_CreateArray();
    if (!arr) { pthread_mutex_unlock(&g_history_mutex); return NULL; }

    if (g_history_count < 2) {
        cJSON_Delete(arr);
        pthread_mutex_unlock(&g_history_mutex);
        return NULL;
    }

    int last = g_history_count - 1;
    cJSON *msg = cJSON_CreateObject();
    if (!msg) { cJSON_Delete(arr); pthread_mutex_unlock(&g_history_mutex); return NULL; }
    cJSON_AddStringToObject(msg, "role", g_history[last].role);
    cJSON_AddStringToObject(msg, "content", g_history[last].content);
    cJSON_AddItemToArray(arr, msg);

    pthread_mutex_unlock(&g_history_mutex);
    return arr;
}

/* ================================================================== */
/*  Transcript — the single FIFO of sections                          */
/* ================================================================== */

typedef struct {
    section_t sections[MAX_SECTIONS];
    int       count;
    int       scroll;
    int       auto_pin;
} transcript_t;

static transcript_t     g_tr;
static pthread_mutex_t  g_tr_mutex = PTHREAD_MUTEX_INITIALIZER;

static int g_focus        = -1;
static int g_user_focused = 0;
static volatile int g_redraw = 1;
static int g_input_active = 0;

static pthread_mutex_t g_req_mutex  = PTHREAD_MUTEX_INITIALIZER;
static int             g_busy       = 0;
static long long       g_busy_since = 0;
static int             g_relay_up   = 0;

volatile int           g_cancel_request = 0;
static char           *g_queued_prompt  = NULL;
static pthread_mutex_t g_queue_mutex    = PTHREAD_MUTEX_INITIALIZER;

static int g_auto_continue_count = 0;

/* ================================================================== */
/*  Runtime counters (status bar)                                     */
/* ================================================================== */

static int g_turn_count = 0;

static pthread_mutex_t g_original_prompt_mutex = PTHREAD_MUTEX_INITIALIZER;
static char            g_original_prompt[INPUT_MAX] = {0};
static int             g_original_prompt_valid = 0;

typedef struct {
    char  label[512];
    int   scroll;
} code_view_t;

code_view_t     g_code;
pthread_mutex_t g_code_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *request_thread(void *arg);

/* Ask the in-flight request to be discarded when it returns. */
static int request_cancel(void) {
    int busy;
    pthread_mutex_lock(&g_req_mutex);
    busy = g_busy;
    pthread_mutex_unlock(&g_req_mutex);

    if (!busy) return 0;

    int was = g_cancel_request;
    g_cancel_request = 1;
    return was ? 0 : 1;          /* 1 only on the first press */
}

/* Hand over `text` (ownership transferred) to be sent as soon as the
 * current request unwinds. Replaces any previously queued prompt. */
static void request_queue_prompt(char *text) {
    pthread_mutex_lock(&g_queue_mutex);
    free(g_queued_prompt);
    g_queued_prompt = text;
    pthread_mutex_unlock(&g_queue_mutex);
}


/* ================================================================== */
/*  Composer / body viewport sizing                                   */
/* ================================================================== */

static int viewport_cap(void) {
    int term = tui_rows();
    int cap  = term * 4 / 10;
    if (cap < 3)  cap = 3;
    if (cap > 12) cap = 12;
    return cap;
}

static int input_rows(int nrows_wrapped) {
    if (!g_input_active) return 0;
    int cap = viewport_cap();
    int h = nrows_wrapped < 1 ? 1 : nrows_wrapped;
    if (h > cap) h = cap;
    return h;
}

/* ---- Section storage -------------------------------------------- */

static void section_free(section_t *s) {
    if (!s) return;
    free(s->label);  s->label  = NULL;
    free(s->body);   s->body   = NULL;
    s->body_len = 0;
}

static void transcript_evict_oldest(void) {
    if (g_tr.count == 0) return;
    section_free(&g_tr.sections[0]);
    memmove(&g_tr.sections[0], &g_tr.sections[1],
            (size_t)(g_tr.count - 1) * sizeof(section_t));
    g_tr.count--;
    if (g_focus > 0) g_focus--;
    else if (g_focus == 0) g_focus = -1;
}

int  section_is_focusable(const section_t *s);
void scroll_focus_into_view(void);

static void transcript_push_locked(sec_kind_t kind, const char *label,
                                   const char *meta, const char *body) {
    if (g_tr.count == MAX_SECTIONS) transcript_evict_oldest();

    section_t *s = &g_tr.sections[g_tr.count++];
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    s->label = strdup(label ? label : "");
    if (!s->label) { g_tr.count--; return; }
    snprintf(s->meta, sizeof(s->meta), "%s", meta ? meta : "");
    if (body && body[0]) {
        size_t n = strlen(body);
        if (n > MAX_BODY_BYTES) n = MAX_BODY_BYTES;
        s->body = malloc(n + 1);
        if (s->body) {
            memcpy(s->body, body, n);
            s->body[n] = '\0';
            s->body_len = n;
        }
    }

    s->open = (kind == SEC_SAYS || kind == SEC_USER) ? 1 : 0;
    s->scroll = 0;

    if (!g_user_focused) {
        g_tr.auto_pin = 1;
        if (section_is_focusable(s)) {
            g_focus = g_tr.count - 1;
        }
    }
}

static void transcript_push_opened(sec_kind_t kind, const char *label,
                                   const char *meta, const char *body,
                                   int open, int scroll) {
    pthread_mutex_lock(&g_tr_mutex);
    transcript_push_locked(kind, label, meta, body);
    if (g_tr.count > 0) {
        section_t *s = &g_tr.sections[g_tr.count - 1];
        s->open   = open;
        s->scroll = scroll;
    }
    pthread_mutex_unlock(&g_tr_mutex);
    wisp_logf("tr   | [%d] %s open=%d scroll=%d (%zu bytes)",
              (int)kind, label ? label : "", open, scroll,
              body ? strlen(body) : 0);
}

/* ---- Console output --------------------------------------------- */

void tr_push_section(sec_kind_t kind, const char *label,
                     const char *meta, const char *body) {
    pthread_mutex_lock(&g_tr_mutex);
    transcript_push_locked(kind, label, meta, body);
    pthread_mutex_unlock(&g_tr_mutex);

    g_redraw = 1;

    wisp_logf("tr   | [%d] %s (%zu bytes)",
            (int)kind, label ? label : "",
            body ? strlen(body) : 0);
}

void tr_push_sectionf(sec_kind_t kind, const char *label,
                      const char *meta, const char *body_fmt, ...) {
    char buf[8192];
    if (body_fmt) {
        va_list ap;
        va_start(ap, body_fmt);
        vsnprintf(buf, sizeof(buf), body_fmt, ap);
        va_end(ap);
    } else {
        buf[0] = '\0';
    }
    tr_push_section(kind, label, meta, buf);
}

void tr_push_user(const char *line) {
    tr_push_section(SEC_USER, "", "", line ? line : "");
}

void tr_push_userf(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0) { va_end(ap2); return; }

    char *buf = malloc((size_t)n + 1);
    if (!buf) { va_end(ap2); return; }
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);

    tr_push_user(buf);
    free(buf);
}

void tr_push_system(const char *line) {
    tr_push_section(SEC_PLAIN, line ? line : "", "", NULL);
}

void tr_push_systemf(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tr_push_system(buf);
}

void tr_push_error(const char *line) {
    tr_push_section(SEC_FAIL, line ? line : "", "", NULL);
}

void tr_push_errorf(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tr_push_error(buf);
}

void tr_push_assistant(const char *line) {
    tr_push_section(SEC_SAYS, "", "", line ? line : "");
}

void tr_push_assistantf(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tr_push_assistant(buf);
}

void tr_push_block_assistant(const char *s) {
    if (!s) return;
    tr_push_assistant(s);
}

void tr_push(const char *line) { tr_push_system(line); }
void tr_pushf(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tr_push_system(buf);
}
void tr_push_block(const char *s) {
    if (!s) return;
    char *copy = strdup(s);
    if (!copy) return;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        tr_push_system(line);
    }
    free(copy);
}

/* ================================================================== */
/*  Ops push — alias for tr_push_section                              */
/* ================================================================== */

void ops_push(const char *line) {
    tr_push_section(SEC_PLAIN, line ? line : "", "", NULL);
}

void ops_pushf(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ops_push(buf);
}

void ops_push_section(sec_kind_t kind, const char *label,
                      const char *meta, const char *body) {
    tr_push_section(kind, label, meta, body);
}

void ops_push_sectionf(sec_kind_t kind, const char *label,
                       const char *meta, const char *body_fmt, ...) {
    char buf[8192];
    if (body_fmt) {
        va_list ap;
        va_start(ap, body_fmt);
        vsnprintf(buf, sizeof(buf), body_fmt, ap);
        va_end(ap);
    } else {
        buf[0] = '\0';
    }
    ops_push_section(kind, label, meta, buf);
}

void ops_push_kind(int kind, const char *line) {
    sec_kind_t k;
    switch (kind) {
        case 0: k = SEC_DONE;    break;
        case 1: k = SEC_PENDING; break;
        case 2: k = SEC_FAIL;    break;
        case 3: k = SEC_IDLE;    break;
        default: k = SEC_PLAIN;  break;
    }
    tr_push_section(k, line ? line : "", "", NULL);
}

void ops_push_kindf(int kind, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ops_push_kind(kind, buf);
}

/* ================================================================== */
/*  OSC 8 hyperlink support                                           */
/* ================================================================== */

static int terminal_supports_osc8(void) {
    int mode = settings_osc8_mode();
    if (mode == 1) return 1;
    if (mode == 2) return 0;

    if (getenv("SSH_CONNECTION") || getenv("SSH_CLIENT") || getenv("SSH_TTY"))
        return 0;

    const char *vte = getenv("VTE_VERSION");
    if (vte && vte[0]) return 0;

    const char *tp = getenv("TERM_PROGRAM");
    if (tp) {
        if (strcmp(tp, "iTerm.app") == 0)   return 1;
        if (strcmp(tp, "WezTerm") == 0)     return 1;
        if (strcmp(tp, "Apple_Terminal") == 0) return 1;
        if (strcmp(tp, "vscode") == 0)      return 1;
        if (strcmp(tp, "Hyper") == 0)       return 1;
        return 1;
    }

    const char *term = getenv("TERM");
    if (term) {
        if (strstr(term, "kitty"))  return 1;
        if (strstr(term, "xterm"))  return 1;
        if (strstr(term, "alacritty")) return 1;
        if (strstr(term, "foot"))   return 1;
    }
    return 0;
}

static void url_encode_append(char *dst, size_t dst_sz, size_t *used,
                              const char *src) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        unsigned char c = *p;
        int safe = (c >= 'a' && c <= 'z') ||
                   (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.' || c == '~' ||
                   c == '/';
        if (safe) {
            if (*used + 1 < dst_sz) dst[(*used)++] = (char)c;
        } else {
            if (*used + 3 < dst_sz) {
                dst[(*used)++] = '%';
                dst[(*used)++] = hex[(c >> 4) & 0xF];
                dst[(*used)++] = hex[c & 0xF];
            }
        }
    }
    dst[*used] = '\0';
}

static void url_append_raw(char *dst, size_t dst_sz, size_t *used,
                           const char *src) {
    size_t n = strlen(src);
    if (*used + n >= dst_sz) n = dst_sz - *used - 1;
    if (n > 0) memcpy(dst + *used, src, n);
    *used += n;
    dst[*used] = '\0';
}

static void ops_push_jump(const char *rel_path, int line) {
    tr_push_code_jump(rel_path, line);
}

static int build_file_url(const char *rel_path, int line,
                          char *out, size_t out_sz) {
    char abs[MAX_PATH_LEN * 2 + 2];
    snprintf(abs, sizeof(abs), "%s/%s", g_project.root, rel_path);

    size_t used = 0;
    out[0] = '\0';
    url_append_raw(out, out_sz, &used, "file://");
    url_encode_append(out, out_sz, &used, abs);

    if (line > 0) {
        char frag[32];
        snprintf(frag, sizeof(frag), "#%d", line);
        url_append_raw(out, out_sz, &used, frag);
    }
    return 0;
}

static int count_lines(const char *buf, size_t len) {
    if (len == 0) return 0;
    int n = 1;
    for (size_t i = 0; i < len; i++) if (buf[i] == '\n') n++;
    if (buf[len - 1] == '\n') n--;
    return n;
}

static int read_file_bytes(const char *abs, char **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    FILE *f = fopen(abs, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0 || sz > MAX_FILE_BYTES) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return 0;
}

static size_t offset_of_line(const char *buf, size_t len, int target) {
    if (target <= 1) return 0;
    int n = 1;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            n++;
            if (n == target) return i + 1;
        }
    }
    return len;
}

void tr_push_code_jump(const char *rel_path, int line) {
    if (!rel_path || !rel_path[0]) return;
    if (line < 1) line = 1;

    char abs[MAX_PATH_LEN * 2 + 2];
    snprintf(abs, sizeof(abs), "%s/%s", g_project.root, rel_path);

    char *filebuf = NULL;
    size_t filelen = 0;
    if (read_file_bytes(abs, &filebuf, &filelen) != 0) {
        char label[1024];
        snprintf(label, sizeof(label), "%s:%d (unreadable)", rel_path, line);
        tr_push_section(SEC_FAIL, label, "", NULL);
        return;
    }

    int total_lines = count_lines(filebuf, filelen);

    char *body = NULL;
    size_t body_len = 0;
    int window_start_line = 1;
    int window_end_line   = total_lines;

    if (filelen <= CODE_SECTION_MAX_BYTES) {
        body = filebuf;
        body_len = filelen;
        filebuf = NULL;
    } else {
        int half = CODE_SECTION_WINDOW / 2;
        int start_line = line - half;
        int end_line   = line + half;
        if (start_line < 1) { end_line += (1 - start_line); start_line = 1; }
        if (end_line > total_lines) {
            start_line -= (end_line - total_lines);
            if (start_line < 1) start_line = 1;
            end_line = total_lines;
        }
        window_start_line = start_line;
        window_end_line   = end_line;

        size_t start_off = offset_of_line(filebuf, filelen, start_line);
        size_t end_off   = offset_of_line(filebuf, filelen, end_line + 1);
        if (end_off > filelen) end_off = filelen;

        body_len = end_off - start_off;
        if (body_len > CODE_SECTION_MAX_BYTES)
            body_len = CODE_SECTION_MAX_BYTES;
        body = malloc(body_len + 1);
        if (!body) { free(filebuf); return; }
        memcpy(body, filebuf + start_off, body_len);
        body[body_len] = '\0';
    }
    free(filebuf);

    char raw_label[1024];
    snprintf(raw_label, sizeof(raw_label), "%s:%d", rel_path, line);

    char labelbuf[4096];
    int label_ok = 0;

    if (terminal_supports_osc8()) {
        char url[MAX_PATH_LEN * 4 + 64];
        build_file_url(rel_path, line, url, sizeof(url));

        int n = snprintf(labelbuf, sizeof(labelbuf),
                         "\x1b]8;;%s\x1b\\%s\x1b]8;;\x1b\\",
                         url, raw_label);
        if (n > 0 && (size_t)n < sizeof(labelbuf)) {
            label_ok = 1;
        } else {
            wisp_logf("code | OSC8 envelope too long (%d > %zu), "
                      "falling back to plain label",
                      n, sizeof(labelbuf));
        }
    }

    if (!label_ok) {
        snprintf(labelbuf, sizeof(labelbuf), "%s", raw_label);
    }

    char meta[64];
    if (filelen <= CODE_SECTION_MAX_BYTES) {
        snprintf(meta, sizeof(meta), "%d / %d · %zu bytes",
                 line, total_lines, body_len);
    } else {
        snprintf(meta, sizeof(meta), "%d-%d / %d · %zu bytes",
                 window_start_line, window_end_line, total_lines, body_len);
    }

    int cols = tui_cols();
    int body_width_eff = cols - 7;
    if (body_width_eff < 8) body_width_eff = 8;

    int initial_scroll = 0;
    {
        int target_local = line - window_start_line + 1;
        size_t off = offset_of_line(body, body_len, target_local);

        int rows = 0;
        int row_len = 0;
        for (size_t i = 0; i < off; i++) {
            if (body[i] == '\n') { rows++; row_len = 0; continue; }
            if (row_len >= body_width_eff) { rows++; row_len = 0; }
            row_len++;
        }
        initial_scroll = rows;
    }

    transcript_push_opened(SEC_CODE, labelbuf, meta, body,
                           1, initial_scroll);
    tr_set_last_hl_path(rel_path);

    free(body);
}

void tr_push_read_file(const char *rel_path) {
    if (!rel_path || !rel_path[0]) return;

    char abs[MAX_PATH_LEN * 2 + 2];
    snprintf(abs, sizeof(abs), "%s/%s", g_project.root, rel_path);

    char *filebuf = NULL;
    size_t filelen = 0;
    if (read_file_bytes(abs, &filebuf, &filelen) != 0) {
        char label[1024];
        snprintf(label, sizeof(label), "read %s (unreadable)", rel_path);
        tr_push_section(SEC_FAIL, label, "", NULL);
        return;
    }

    size_t body_len = filelen;
    if (body_len > MAX_BODY_BYTES) body_len = MAX_BODY_BYTES;

    char label[1024];
    snprintf(label, sizeof(label), "read %s", rel_path);

    char meta[64];
    snprintf(meta, sizeof(meta), "%zu bytes", filelen);

    tr_push_section(SEC_READ, label, meta, filebuf);
    tr_set_last_hl_path(rel_path);
    free(filebuf);
}

/* ================================================================== */
/*  Code view — stub                                                  */
/* ================================================================== */

void code_clear(void) {
    pthread_mutex_lock(&g_code_mutex);
    g_code.label[0] = '\0';
    g_code.scroll = 0;
    pthread_mutex_unlock(&g_code_mutex);
}

void code_set(const char *label, const char *text) {
    pthread_mutex_lock(&g_code_mutex);
    snprintf(g_code.label, sizeof(g_code.label), "%s", label ? label : "");
    g_code.scroll = 0;
    pthread_mutex_unlock(&g_code_mutex);

    if (text && text[0]) {
        tr_push_section(SEC_IDLE, label ? label : "output", "", text);
    }
}

int code_load_file(const char *rel) {
    if (!rel || !rel[0]) return -1;

    char abs[MAX_PATH_LEN * 2 + 2];
    snprintf(abs, sizeof(abs), "%s/%s", g_project.root, rel);

    FILE *f = fopen(abs, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0 || sz > MAX_FILE_BYTES) {
        fclose(f);
        return -1;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';

    pthread_mutex_lock(&g_code_mutex);
    snprintf(g_code.label, sizeof(g_code.label), "%s", rel);
    pthread_mutex_unlock(&g_code_mutex);

    free(buf);
    return 0;
}

void code_jump_to(const char *rel_path, int line) {
    if (!rel_path || !rel_path[0]) return;
    if (line < 1) line = 1;

    if (code_load_file(rel_path) != 0) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "jump: could not open %s", rel_path);
        tr_push_section(SEC_FAIL, buf, "", NULL);
        return;
    }

    ops_push_jump(rel_path, line);
    wisp_logf("jump | %s:%d", rel_path, line);
}

/* ================================================================== */
/*  Run (Alt-B)                                                       */
/* ================================================================== */
#if defined(_WIN32)

static char *run_capture(const char *cmd, int *exit_code) {
    if (exit_code) *exit_code = -1;

    /* _popen on Windows already runs the command through cmd.exe /c.
     * Adding our own cmd.exe /c nests a second shell and confuses the
     * && chain — the inner cmd eats the rest of the line as its own
     * argument and the outer one never sees the run command. Just pass
     * the command, wrapped in a subshell so the 2>&1 applies to every
     * command inside it (including the cd). */
    char full[MAX_PATH_LEN * 4];
    snprintf(full, sizeof(full), "(%s) 2>&1", cmd);

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

#else

static char *run_capture(const char *cmd, int *exit_code) {
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
#if defined(WISP_TERMUX) || defined(__ANDROID__)
        const char *shell = getenv("PREFIX");
        static char shellbuf[512];
        if (shell && shell[0]) {
            snprintf(shellbuf, sizeof(shellbuf), "%s/bin/sh", shell);
            shell = shellbuf;
        } else {
            shell = "/system/bin/sh";
        }
#else
        const char *shell = "/bin/sh";
#endif
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

#endif

static void *build_thread(void *arg) {
    (void)arg;

    run_conf_get(g_project.root, "run", g_build_run, sizeof(g_build_run));

    if (!g_build_run[0]) {
        tr_push_section(SEC_IDLE, "no run command configured", "", NULL);
        tr_push_system("[no runtime set for this project]");
        tr_push_system("[ask the model how to build or run it]");
        g_last_error = 0;

        pthread_mutex_lock(&g_req_mutex);
        g_busy = 0;
        g_busy_since = 0;
        pthread_mutex_unlock(&g_req_mutex);
        g_redraw = 1;
        tr_focus_tail();
        return NULL;
    }

    char cmd[MAX_PATH_LEN * 4];
#if defined(_WIN32)
    /* cmd.exe uses double quotes, and /d switches the drive too —
     * g_project.root can be on D: while the shell started on C:.
     * Single quotes are literal characters to cmd.exe and would make
     * cd look for a directory whose name contains the quote marks. */
    snprintf(cmd, sizeof(cmd), "cd /d \"%s\" && %s",
             g_project.root, g_build_run);
#else
    snprintf(cmd, sizeof(cmd), "cd '%s' && %s",
             g_project.root, g_build_run);
#endif

    int ec = 0;
    char *out = run_capture(cmd, &ec);

    char meta[64];
    snprintf(meta, sizeof(meta), "exit %d", ec);

    transcript_push_opened(SEC_RUN, g_build_run, meta,
                           out ? out : NULL,
                           1, 0);

    if (ec != 0) {
        g_last_error = 1;
        if (out && out[0]) {
            free(g_pending_run_output);
            g_pending_run_output = strdup(out);
        }
    } else {
        free(g_pending_run_output);
        g_pending_run_output = NULL;
    }

    free(out);

    pthread_mutex_lock(&g_req_mutex);
    g_busy = 0;
    g_busy_since = 0;
    pthread_mutex_unlock(&g_req_mutex);
    tr_focus_tail();
    g_redraw = 1;
    return NULL;
}

static void start_build(void) {
    pthread_mutex_lock(&g_req_mutex);
    if (g_busy) {
        pthread_mutex_unlock(&g_req_mutex);
        tr_push_section(SEC_PENDING, "busy — wait for the current request",
                        "", NULL);
        return;
    }
    g_busy = 1;
    g_busy_since = monotonic_ms();
    g_last_error = 0;
    pthread_mutex_unlock(&g_req_mutex);

    pthread_t tid;
    if (pthread_create(&tid, NULL, build_thread, NULL) != 0) {
        tr_push_section(SEC_FAIL, "could not spawn run thread", "", NULL);
        pthread_mutex_lock(&g_req_mutex);
        g_busy = 0;
        g_busy_since = 0;
        pthread_mutex_unlock(&g_req_mutex);
        return;
    }
    pthread_detach(tid);
}

/* ================================================================== */
/*  Request worker                                                    */
/* ================================================================== */

static void log_request_body(const char *body, size_t body_len) {
    wisp_logf("req  | POST body %zu bytes", body_len);

    if (g_shell_mode) {
        static char reads[65536];
        snapshot_emit_reads(reads, sizeof(reads));
        if (reads[0]) {
            wisp_logf("req  | [READS]:");
            char *copy = strdup(reads);
            if (copy) {
                char *save = NULL;
                for (char *line = strtok_r(copy, "\n", &save);
                     line; line = strtok_r(NULL, "\n", &save)) {
                    wisp_logf("req  |   %.200s", line);
                }
                free(copy);
            }
        } else {
            wisp_logf("req  | [READS]: none");
        }
        wisp_logf("req  | shell mode — no snapshot, no delta");
    } else {
        static char changes[65536];
        snapshot_emit_delta(changes, sizeof(changes));
        if (changes[0]) {
            wisp_logf("req  | [CHANGES]:");
            char *copy = strdup(changes);
            if (copy) {
                char *save = NULL;
                for (char *line = strtok_r(copy, "\n", &save);
                     line; line = strtok_r(NULL, "\n", &save)) {
                    wisp_logf("req  |   %s", line);
                }
                free(copy);
            }
        } else {
            wisp_logf("req  | [CHANGES]: none");
        }
        wisp_logf("req  | %s snapshot in turn",
                   g_project.last_sent_valid ? "delta-only, no" : "full");
    }

    size_t preview_len = body_len > 4096 ? 4096 : body_len;
    wisp_logf("req  | body preview (first %zu bytes):", preview_len);
    wisp_log_raw(body, preview_len);
    if (body_len > preview_len) {
        wisp_logf("\nreq  | ... (%zu more bytes not shown)",
                   body_len - preview_len);
    }

    {
        size_t total = 0;
        int count = 0;
        pthread_mutex_lock(&g_history_mutex);
        count = g_history_count;
        for (int i = 0; i < g_history_count; i++) {
            if (g_history[i].content) total += strlen(g_history[i].content);
        }
        pthread_mutex_unlock(&g_history_mutex);
        wisp_logf("req  | history: %d messages, %zu bytes", count, total);
    }

    wisp_logf("req  | --- end body preview ---");
}

static void spawn_request(const char *text) {
    char *copy = strdup(text);
    if (!copy) {
        pthread_mutex_lock(&g_req_mutex);
        g_busy = 0;
        g_busy_since = 0;
        pthread_mutex_unlock(&g_req_mutex);
        tr_push_section(SEC_FAIL, "request failed: out of memory", "", NULL);
        return;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, request_thread, copy) != 0) {
        free(copy);
        pthread_mutex_lock(&g_req_mutex);
        g_busy = 0;
        g_busy_since = 0;
        pthread_mutex_unlock(&g_req_mutex);
        tr_push_section(SEC_FAIL, "request failed: could not spawn worker",
                        "", NULL);
        return;
    }
    pthread_detach(tid);
}

static void *request_thread(void *arg) {
    char *prompt = (char *)arg;

    g_last_reply_bytes = 0;
    g_user_focused = 0;

    int is_read_continue =
        (strcmp(prompt, "(auto-continuing after READ)") == 0);

    if (!is_read_continue) {
        tr_push_userf("%s", prompt);
    }

    if (!g_shell_mode) {
        if (build_snapshot() != 0) {
            tr_push_section(SEC_FAIL, "snapshot build failed", "", NULL);
            tr_push_error("[could not build project snapshot]");
            free(prompt);
            snapshot_capture_baseline();
            pthread_mutex_lock(&g_req_mutex);
            g_busy = 0;
            g_busy_since = 0;
            pthread_mutex_unlock(&g_req_mutex);
            tr_focus_tail();
            g_redraw = 1;
            return NULL;
        }
    }

    const char *effective_prompt = prompt;
    char effective_buf[INPUT_MAX];
    if (is_read_continue) {
        pthread_mutex_lock(&g_original_prompt_mutex);
        if (g_original_prompt_valid) {
            snprintf(effective_buf, sizeof(effective_buf), "%s",
                     g_original_prompt);
            effective_prompt = effective_buf;
        }
        pthread_mutex_unlock(&g_original_prompt_mutex);
    }

    wisp_logf("req  | prompt: %.100s", effective_prompt);
    char *body = build_request_body(effective_prompt);
    free(prompt);

    if (!body) {
        tr_push_section(SEC_FAIL, "request build failed", "", NULL);
        tr_push_error("[could not build request body]");
        if (!g_shell_mode) snapshot_capture_baseline();
        pthread_mutex_lock(&g_req_mutex);
        g_busy = 0;
        g_busy_since = 0;
        pthread_mutex_unlock(&g_req_mutex);
        tr_focus_tail();
        g_redraw = 1;
        return NULL;
    }

    log_request_body(body, strlen(body));

    g_redraw = 1;

    http_reply_t r;
    int rc = http_post_json(body, &r, settings_timeout_ms());
    free(body);

    read_cache_clear();

    if (rc == -2) {
        if (r.body) free(r.body);
        if (!g_shell_mode) snapshot_capture_baseline();

        char *queued = NULL;
        pthread_mutex_lock(&g_queue_mutex);
        queued = g_queued_prompt;
        g_queued_prompt = NULL;
        pthread_mutex_unlock(&g_queue_mutex);

        if (queued) {
            tr_push_section(SEC_PENDING,
                            "cancelled — sending queued message", "", NULL);
            g_busy_since = monotonic_ms();
            spawn_request(queued);
            free(queued);
            return NULL;
        }

        tr_push_section(SEC_PENDING, "cancelled", "", NULL);
        pthread_mutex_lock(&g_req_mutex);
        g_busy = 0;
        g_busy_since = 0;
        pthread_mutex_unlock(&g_req_mutex);
        tr_focus_tail();
        g_redraw = 1;
        return NULL;
    }

    int process_rc = REPLY_OK;

    if (rc != 0 || !r.body) {
        char buf[128];
        snprintf(buf, sizeof(buf), "upstream returned nothing (rc=%d)", rc);
        tr_push_section(SEC_FAIL, buf, "", NULL);
        wisp_logf("req  | upstream returned nothing, rc=%d", rc);
        g_last_error = 1;
    } else {
        wisp_logf("req  | reply %zu bytes, status %d", r.len, r.status);
        g_last_reply_bytes = (int)r.len;

        if (r.status >= 400) {
            char *err = extract_content(r.body);
            if (err) {
                tr_push_errorf("[upstream error %d] %s", r.status, err);
                free(err);
            } else {
                tr_push_errorf("[upstream error %d]", r.status);
                tr_push_block(r.body);
            }
            g_last_error = 1;
        } else {
            char *content = extract_content(r.body);
            if (!content) {
                tr_push_error("[upstream reply had no content field]");
                g_last_error = 1;
            } else {
                wisp_logf("req  | extracted content %zu bytes",
                           strlen(content));

                history_append_assistant(content);

                if (!g_shell_mode) {
                    build_snapshot();
                }

                process_rc = process_reply(content);

                if (!g_shell_mode) {
                    build_snapshot();
                    snapshot_capture_baseline();
                }

                free(content);
                g_turn_count++;
            }
        }
        free(r.body);
    }

    run_conf_get(g_project.root, "run", g_build_run, sizeof(g_build_run));

    if (process_rc == REPLY_READ_NEXT) {
        pthread_mutex_lock(&g_req_mutex);
        g_busy = 1;
        g_busy_since = monotonic_ms();
        pthread_mutex_unlock(&g_req_mutex);
        g_redraw = 1;

        g_auto_continue_count++;
        spawn_request("(auto-continuing after READ)");
        return NULL;
    }

    pthread_mutex_lock(&g_original_prompt_mutex);
    g_original_prompt_valid = 0;
    g_original_prompt[0] = '\0';
    pthread_mutex_unlock(&g_original_prompt_mutex);

    pthread_mutex_lock(&g_req_mutex);
    g_busy = 0;
    g_busy_since = 0;
    pthread_mutex_unlock(&g_req_mutex);
    tr_focus_tail();
    g_redraw = 1;

    return NULL;
}

/* ================================================================== */
/*  Colour helpers                                                    */
/* ================================================================== */

static const char *color_for_kind(sec_kind_t k) {
    switch (k) {
        case SEC_DONE:         return C_OK;
        case SEC_PENDING:      return C_WARN;
        case SEC_FAIL:         return C_ERR;
        case SEC_IDLE:         return C_MUTED;
        case SEC_SAYS:         return C_AI;
        case SEC_USER:         return C_USER;
        case SEC_CODE:         return C_PATH;
        case SEC_READ:         return C_PATH;
        case SEC_BANNER_WORD:  return C_ACCENT;
        case SEC_BANNER_CODE:  return C_TITLE;
        case SEC_BANNER_SPARK: return C_MUTED;
        case SEC_RUN:          return C_ACCENT;
        case SEC_PLAIN:
        default:               return C_INFO;
    }
}

static const char *pill_for_kind(sec_kind_t k) {
    switch (k) {
        case SEC_DONE:    return "[DONE]";
        case SEC_PENDING: return "[PENDING]";
        case SEC_FAIL:    return "[FAILED]";
        case SEC_IDLE:    return "[IDLE]";
        case SEC_SAYS:    return "[MSG]";
        case SEC_USER:    return "[USER]";
        case SEC_CODE:    return "[CODE]";
        case SEC_READ:    return "[READ]";
        case SEC_RUN:     return "[RUN]";
        default:          return "";
    }
}

/* ================================================================== */
/*  Border helpers                                                    */
/* ================================================================== */

static void rule_plain(int row, int cols) {
    tui_goto(row, 1);
    for (int c = 1; c <= cols; c++) {
        emit_all(C_BORDER, BX_H, C_RESET, NULL);
    }
}

/* ================================================================== */
/*  Status bar                                                        */
/* ================================================================== */
static void draw_status_bar(int row, int cols) {
    int busy;
    pthread_mutex_lock(&g_req_mutex);
    busy = g_busy;
    pthread_mutex_unlock(&g_req_mutex);

    const char *dot_col;
    const char *dot_txt;
    static char busybuf[64];
    if (!g_relay_up) {
        dot_col = C_DIM;
        dot_txt = "○ offline";
    } else if (busy) {
        dot_col = C_ACCENT;
        snprintf(busybuf, sizeof(busybuf), "● thinking");
        dot_txt = busybuf;
    } else {
        dot_col = C_OK;
        dot_txt = "● ready";
    }

    size_t hb  = history_bytes();
    size_t cap = HISTORY_MAX_BYTES;

    size_t est_tokens_k = (hb / CHARS_PER_TOKEN) / 1000;

    char ctxbuf[64];
    if (hb >= 1024) {
        snprintf(ctxbuf, sizeof(ctxbuf), "ctx %.1fk ~%zuk/%zuk",
                 (double)hb / 1024.0,
                 est_tokens_k,
                 cap / 1024);
    } else {
        snprintf(ctxbuf, sizeof(ctxbuf), "ctx %zuB ~0k/%zuk",
                 hb, cap / 1024);
    }

    const char *ctx_col = C_MUTED;
    if (hb >= cap * 9 / 10)      ctx_col = C_ERR;
    else if (hb >= cap * 7 / 10) ctx_col = C_WARN;

    const char *mode = g_shell_mode ? "shell" : "code";
    const char *theme_name = theme_current()->name;

    const char *sep = " │ ";
    int sep_w = disp_width(sep);

    int right_w = disp_width("mode: ") + disp_width(mode)
                + sep_w
                + disp_width(theme_name) + 1;

    tui_puts_at_padded(row, 1, cols, "");

    int cursor = 1;

    tui_goto(row, cursor);
    write_str(dot_col);
    write_str(dot_txt);
    write_str(C_RESET);
    cursor += disp_width(dot_txt);

    tui_goto(row, cursor);
    write_str(C_BORDER); write_str(sep); write_str(C_RESET);
    cursor += sep_w;

    const char *model = settings_model();
    if (!model || !model[0]) model = "none";

    char ws_buf[128];
    snprintf(ws_buf, sizeof(ws_buf), "%s", model);

    tui_goto(row, cursor);
    write_str(C_MUTED); write_str(ws_buf); write_str(C_RESET);
    cursor += disp_width(ws_buf);

    tui_goto(row, cursor);
    write_str(C_BORDER); write_str(sep); write_str(C_RESET);
    cursor += sep_w;

    tui_goto(row, cursor);
    write_str(ctx_col); write_str(ctxbuf); write_str(C_RESET);
    cursor += disp_width(ctxbuf);

    int right_start = cols - right_w + 1;
    if (right_start < cursor + 2) right_start = cursor + 2;
    if (right_start < 1) right_start = 1;
    if (right_start > cols) return;

    cursor = right_start;

    tui_goto(row, cursor);
    write_str(C_MUTED); write_str("mode: "); write_str(mode); write_str(C_RESET);
    cursor += disp_width("mode: ") + disp_width(mode);

    tui_goto(row, cursor);
    write_str(C_BORDER); write_str(sep); write_str(C_RESET);
    cursor += sep_w;

    tui_goto(row, cursor);
    write_str(C_ACCENT); write_str(C_BOLD); write_str(theme_name); write_str( " " ); write_str(C_RESET);

    (void)cursor;
}

/* ================================================================== */
/*  Wrap helpers                                                      */
/* ================================================================== */

typedef struct {
    int start;
    int len;
    int is_break;
} wrap_row_t;

static int wrap_compute(const char *input, int width, wrap_row_t *rows, int max_rows) {
    int n = 0;
    if (width < 2) width = 2;
    if (max_rows < 1) return 0;

    int i = 0;
    int row_start = 0;
    int row_len = 0;

    while (input[i] && n < max_rows) {
        if (input[i] == '\n') {
            rows[n].start = row_start;
            rows[n].len = row_len;
            rows[n].is_break = 1;
            n++;
            i++;
            row_start = i;
            row_len = 0;
            continue;
        }
        if (row_len >= width) {
            rows[n].start = row_start;
            rows[n].len = row_len;
            rows[n].is_break = 0;
            n++;
            row_start = i;
            row_len = 0;
            if (n >= max_rows) break;
            continue;
        }
        i++;
        row_len++;
    }
    if (n < max_rows) {
        rows[n].start = row_start;
        rows[n].len = row_len;
        rows[n].is_break = 0;
        n++;
    }
    return n;
}

static void wrap_locate(const wrap_row_t *rows, int nrows, int offset,
                        int *out_row, int *out_col) {
    for (int r = 0; r < nrows; r++) {
        int end = rows[r].start + rows[r].len;
        if (r == nrows - 1) {
            if (offset >= rows[r].start && offset <= end) {
                *out_row = r;
                *out_col = offset - rows[r].start;
                return;
            }
        } else {
            int limit = end + (rows[r].is_break ? 1 : 0);
            if (offset >= rows[r].start && offset < limit) {
                *out_row = r;
                *out_col = offset - rows[r].start;
                if (*out_col > rows[r].len) *out_col = rows[r].len;
                return;
            }
        }
    }
    *out_row = nrows - 1;
    *out_col = rows[nrows - 1].len;
}

/* ================================================================== */
/*  Transcript renderer                                               */
/* ================================================================== */

typedef struct {
    int row;
    int bot;
    int cols;
    int skip_above;
} sec_view_t;

static int section_body_rows(const section_t *s, int body_width) {
    if (!s->body || s->body_len == 0) return 0;
    if (body_width < 8) body_width = 8;

    int rows = 0;
    size_t i = 0;

    while (i < s->body_len) {
        rows++;

        if (s->body[i] == '\n') {
            i++;
            continue;
        }

        int len = 0;
        int last_ws = -1;
        while (i + (size_t)len < s->body_len &&
               s->body[i + len] != '\n' &&
               len < body_width) {
            char c = s->body[i + len];
            if (c == ' ' || c == '\t') last_ws = len;
            len++;
        }

        int is_nl = (i + len < s->body_len && s->body[i + len] == '\n');

        if (is_nl) {
            i += (size_t)len + 1;
        } else if (len == body_width && last_ws > 0) {
            i += (size_t)last_ws + 1;
        } else {
            i += (size_t)len;
        }
    }

    return rows;
}

static int kind_is_plain_like(sec_kind_t k) {
    return (k == SEC_PLAIN
         || k == SEC_BANNER_WORD
         || k == SEC_BANNER_CODE
         || k == SEC_BANNER_SPARK);
}

static int body_width_for(int cols) {
    int w = cols - 7;
    if (w < 8) w = 8;
    return w;
}

static int section_is_foldable(const section_t *s, int body_width) {
    int rows = section_body_rows(s, body_width);

    if (s->kind == SEC_SAYS) return (rows > 0);
    if (s->kind == SEC_USER) return (rows >= 2);
    if (s->kind == SEC_RUN)  return (rows > 0);
    if (s->kind == SEC_FAIL) return (rows > 0);
    if (s->kind == SEC_DONE) return (rows >= 2);

    return (rows >= 3);
}

int section_is_focusable(const section_t *s) {
    if (kind_is_plain_like(s->kind)) return 0;
    int cols = tui_cols();
    int body_width = body_width_for(cols);
    return section_is_foldable(s, body_width);
}

#define FOCUS_BODY_CAP 12

static int focus_viewport_cap(void) {
    int term = tui_rows();
    int natural = term * 6 / 10;
    int cap = FOCUS_BODY_CAP;
    if (cap > natural) cap = natural;
    if (cap < 3) cap = 3;
    return cap;
}

static int body_effective_cap(void) {
    int transcript_room = g_body_visible - 1;
    if (transcript_room < 1) transcript_room = 1;

    int cap = g_cap_in_focus ? focus_viewport_cap() : viewport_cap();
    if (transcript_room < cap) cap = transcript_room;
    return cap;
}

static int body_visible_cap(int total_rows) {
    if (total_rows <= 0) return 0;
    int cap = body_effective_cap();
    return total_rows > cap ? cap : total_rows;
}

static int body_rows_to_draw(const section_t *s, int total_rows) {
    if (total_rows <= 0) return 0;
    int cap = body_visible_cap(total_rows);
    int vis = total_rows - s->scroll;
    if (vis > cap) vis = cap;
    if (vis < 0) vis = 0;
    return vis;
}

static int section_height(const section_t *s, int body_width) {
    if (kind_is_plain_like(s->kind)) {
        int avail = body_width + 4;           /* == cols - 3 */
        if (avail < 1) avail = 1;
        int plen = (int)strlen(s->label);
        if (plen == 0) return 1;

        int prefix = (s->kind == SEC_USER) ? 2 : 0;
        int cap = avail - prefix;
        if (cap < 1) cap = 1;

        int rows = 0;
        int pos  = 0;
        while (pos < plen) {
            int take = 0;
            while (pos + take < plen && take < cap &&
                   s->label[pos + take] != '\n') take++;
            rows++;
            pos += take;
            if (pos < plen && s->label[pos] == '\n') pos++;
        }
        if (rows < 1) rows = 1;
        return rows;
    }

    int body_rows = section_body_rows(s, body_width);
    if (body_rows == 0) return 1;

    if (!section_is_foldable(s, body_width)) {
        return 1;
    }

    if (!s->open) return 1;
    return 1 + body_rows_to_draw(s, body_rows);
}

static int body_next_line(const section_t *s, int body_width,
                          size_t *cursor, char *out, int out_sz) {
    if (*cursor >= s->body_len) return 0;

    size_t i = *cursor;
    int len = 0;

    int last_ws = -1;
    while (i + (size_t)len < s->body_len &&
           s->body[i + len] != '\n' &&
           len < body_width) {
        char c = s->body[i + len];
        if (c == ' ' || c == '\t') last_ws = len;
        len++;
    }

    int is_nl = (i + len < s->body_len && s->body[i + len] == '\n');

    int emit_len = len;
    int consume_ws = 0;
    if (!is_nl && len == body_width && last_ws > 0) {
        emit_len = last_ws;
        consume_ws = 1;
    }

    int n = emit_len;
    if (n > out_sz - 1) n = out_sz - 1;
    memcpy(out, s->body + i, (size_t)n);
    out[n] = '\0';

    *cursor = i + (size_t)emit_len + (consume_ws ? 1u : 0u)
            + (is_nl ? 1u : 0u);
    return 1;
}

static int draw_section(const sec_view_t *v, const section_t *s,
                        int is_focused, int row) {
    if (row > v->bot) return row;

    int skip = v->skip_above;

    if (kind_is_plain_like(s->kind)) {
        int indent = 3;
        int avail  = v->cols - indent;
        if (avail < 1) avail = 1;

        const char *col = color_for_kind(s->kind);
        const char *p   = s->label;
        int plen = (int)strlen(p);
        int pos  = 0;

        if (plen == 0) {
            if (skip > 0) return row;
            if (row <= v->bot) {
                tui_goto(row, 1);
                write_str("   ");
                row++;
            }
            return row;
        }

        while (pos < plen && row <= v->bot) {
            int cap = avail;
            if (cap < 1) cap = 1;

            int take   = 0;
            int hit_nl = 0;
            while (pos + take < plen && take < cap) {
                if (p[pos + take] == '\n') { hit_nl = 1; break; }
                take++;
            }

            if (skip > 0) {
                skip--;
                pos += take + hit_nl;
                continue;
            }

            tui_goto(row, 1);
            write_str("   ");

            if (take > 0) {
                tui_goto(row, indent + 1);
                write_str(col);
                { ssize_t w = write(STDOUT_FILENO, p + pos, (size_t)take); (void)w; }
                write_str(C_RESET);
            }

            pos += take + hit_nl;
            row++;
        }
        return row;
    }

    const char *col  = color_for_kind(s->kind);
    const char *pill = pill_for_kind(s->kind);

    int body_width = body_width_for(v->cols);

    int body_rows = section_body_rows(s, body_width);
    int foldable  = section_is_foldable(s, body_width);
    int inline_one_liner = (body_rows > 0 && !foldable);

    if (skip > 0) {
        skip--;
    } else {
        tui_goto(row, 1);
        if (is_focused) {
            write_str(C_BOLD);
            write_str(C_ACCENT);
            write_str("▸");
            write_str(C_RESET);
        } else {
            write_str(col);
            write_str("┃");
            write_str(C_RESET);
        }

        int cursor = 2;

        tui_goto(row, cursor); write_str(" "); cursor++;

        if (foldable) {
            tui_goto(row, cursor);
            if (s->open) {
                write_str(C_BOLD); write_str(C_ACCENT); write_str("[-]");
            } else {
                write_str(C_MUTED); write_str("[+]");
            }
            write_str(C_RESET);
            cursor += 3;

            tui_goto(row, cursor); write_str(" "); cursor++;
        }

        if (pill[0]) {
            tui_goto(row, cursor);
            write_str(col);
            write_str(pill);
            write_str(C_RESET);
            cursor += (int)strlen(pill);
            tui_goto(row, cursor);
            write_str(" ");
            cursor++;
        }

        int meta_w = (int)strlen(s->meta);
        int label_avail = v->cols - cursor + 1;
        int label_w = label_avail - (meta_w > 0 ? meta_w + 2 : 0);
        if (label_w < 1) label_w = 1;

        char labelbuf[2048];
        snprintf(labelbuf, sizeof(labelbuf), "%s", s->label);

        if (inline_one_liner) {
            size_t n = strlen(labelbuf);
            if (n < sizeof(labelbuf) - 2) {
                if (n > 0) labelbuf[n++] = ' ';
                size_t i = 0;
                while (i < s->body_len && s->body[i] != '\n' &&
                       n < sizeof(labelbuf) - 1) {
                    labelbuf[n++] = s->body[i++];
                }
                labelbuf[n] = '\0';
            }
        }

        char fitted[2048];
        int lw = 0;
        const char *lp = labelbuf;
        while (*lp && lw < label_w) {
            unsigned char c = (unsigned char)*lp;
            int adv = ((c & 0x80) == 0x00) ? 1
                    : ((c & 0xE0) == 0xC0) ? 2
                    : ((c & 0xF0) == 0xE0) ? 3
                    : ((c & 0xF8) == 0xF0) ? 4
                    : 1;
            if (lw + 1 > label_w) break;
            memcpy(fitted + (lp - labelbuf), lp, (size_t)adv);
            lp += adv;
            lw++;
        }
        fitted[lp - labelbuf] = '\0';

        tui_goto(row, cursor);
        if (is_focused) {
            write_str(C_BOLD); write_str(C_ACCENT);
        } else if (s->kind == SEC_SAYS) {
            write_str(C_AI);
        } else if (s->kind == SEC_USER) {
            write_str(C_USER);
        } else {
            write_str(C_INFO);
        }
        write_str(fitted);
        write_str(C_RESET);
        for (int k = lw; k < label_w; k++) write_str(" ");

        char metabuf[64];
        const char *meta_out = s->meta;
        if (foldable && s->open) {
            int total = body_rows;
            int vis   = body_rows_to_draw(s, total);
            if (total > vis) {
                int top = s->scroll + 1;
                int bot = s->scroll + vis;
                snprintf(metabuf, sizeof(metabuf), "%d-%d/%d", top, bot, total);
                meta_out = metabuf;
            }
        }
        int meta_out_w = (int)strlen(meta_out);
        if (meta_out_w > 0) {
            int meta_start = v->cols - meta_out_w + 1;
            if (meta_start < 1) meta_start = 1;
            tui_goto(row, meta_start);
            write_str(C_MUTED);
            write_str(meta_out);
            write_str(C_RESET);
        }

        row++;
    }

    if (body_rows == 0) return row;
    if (inline_one_liner) return row;
    if (!foldable)      return row;
    if (!s->open)       return row;

    int body_left = 4;
    int body_width_eff = v->cols - body_left - 3;
    if (body_width_eff < 8) body_width_eff = 8;

    int vis = body_rows_to_draw(s, body_rows) - skip;
    if (vis < 0) vis = 0;

    int skip_body = s->scroll + skip;
    size_t i = 0;
    int drawn = 0;
    int first = (skip_body == s->scroll);
    char linebuf[2048];

    hl_lang_t hl_lang = HL_LANG_NONE;
    if (s->hl_diff) {
        if (s->hl_path[0]) {
            hl_lang = hl_lang_for_path(s->hl_path);
        }
    } else if ((s->kind == SEC_CODE || s->kind == SEC_READ) &&
               s->hl_path[0]) {
        hl_lang = hl_lang_for_path(s->hl_path);
    }

    void *hl = hl_lang != HL_LANG_NONE ? hl_state_new(hl_lang) : NULL;
    hl_diff_state_t diff_state = HL_DIFF_NEUTRAL;

    while (drawn < vis && row <= v->bot) {
        if (!body_next_line(s, body_width_eff, &i, linebuf, sizeof(linebuf)))
            break;

        if (skip_body > 0) {
            if (s->hl_diff) {
                hl_emit_diff_line(&diff_state, hl, linebuf,
                                  (int)strlen(linebuf), 1);
            } else if (hl) {
                hl_emit_line(hl, linebuf, (int)strlen(linebuf), 1);
            }
            skip_body--;
            first = 0;
            continue;
        }

        tui_goto(row, 1);
        write_str(col);
        write_str("┃");
        write_str(C_RESET);

        if (first) {
            tui_goto(row, body_left);
            write_str(C_BORDER); write_str("└─ "); write_str(C_RESET);
        } else {
            tui_goto(row, body_left);
            write_str("   ");
        }

        tui_goto(row, body_left + 3);

        if (s->hl_diff) {
            hl_emit_diff_line(&diff_state, hl, linebuf,
                              (int)strlen(linebuf), 0);
        } else if (hl) {
            hl_emit_line(hl, linebuf, (int)strlen(linebuf), 0);
        } else {
            write_str(C_INFO);
            write_str(linebuf);
            write_str(C_RESET);
        }

        row++;
        first = 0;
        drawn++;
    }

    hl_state_free(hl);

    return row;
}

/* ================================================================== */
/*  Transcript renderer — viewport                                    */
/* ================================================================== */
static void render_transcript(int top, int bot, int cols) {
    pthread_mutex_lock(&g_tr_mutex);

    int body_width = body_width_for(cols);

    int total = 0;
    for (int i = 0; i < g_tr.count; i++) {
        int was = g_cap_in_focus;
        g_cap_in_focus = (g_focus == i);
        total += section_height(&g_tr.sections[i], body_width);
        g_cap_in_focus = was;
    }

    int visible = bot - top + 1;
    if (visible < 1) {
        pthread_mutex_unlock(&g_tr_mutex);
        return;
    }

    int max_off = total > visible ? total - visible : 0;
    int scroll = g_tr.scroll;
    if (g_tr.auto_pin) scroll = max_off;
    if (scroll > max_off) scroll = max_off;
    if (scroll < 0) scroll = 0;

    int draw_row = top;
    int y = 0;

    sec_view_t v;
    v.row = top;
    v.bot = bot;
    v.cols = cols;
    v.skip_above = 0;

    for (int i = 0; i < g_tr.count && draw_row <= bot; i++) {
        int focus_here = (g_focus == i);

        g_cap_in_focus = focus_here;
        int h = section_height(&g_tr.sections[i], body_width);
        g_cap_in_focus = 0;

        int skip_above = 0;
        if (y < scroll) {
            skip_above = scroll - y;
            if (skip_above > h) skip_above = h;
        }

        if (skip_above >= h) {
            y += h;
            continue;
        }

        v.row = draw_row;
        v.bot = bot;
        v.cols = cols;
        v.skip_above = skip_above;

        g_cap_in_focus = focus_here;
        int after = draw_section(&v, &g_tr.sections[i], focus_here, draw_row);
        g_cap_in_focus = 0;

        draw_row = after;
        y += h;
    }

    while (draw_row <= bot) {
        tui_puts_at_padded(draw_row, 1, cols, "");
        draw_row++;
    }

    pthread_mutex_unlock(&g_tr_mutex);
}

/* ================================================================== */
/*  Hint bar — shortcuts only, no corners                             */
/* ================================================================== */

static void rule_bottom_with_hint(int row, int cols, int mode) {
    static const char HINT_IDLE[] =
        " Alt-B · Run ─ Alt-T · Theme ─ Alt-S · Settings ─ Tab · Focus ─ ^C · Quit ";
    static const char HINT_FOCUS[] =
        " Tab · Next ─ Shift+Tab · Prev ─ Enter · Toggle ─ Arrows · Scroll ─ Esc · Unfocus ";
    static const char HINT_BODY[] =
        " PgUp/PgDn · Scroll body ─ Arrows · Section ─ Enter · Collapse ─ Esc · Close ";
    static const char HINT_INPUT[] =
        " Enter · Send ─ Alt+Enter · Newline ─ Esc · Close ";

    static const char HINT_BUSY[] =
        " Esc · Cancel ─ Enter · Queue ─ ^C · Quit ";

    const char *HINT;
    switch (mode) {
        case 4:  HINT = HINT_BUSY;  break;
        case 3:  HINT = HINT_BODY;  break;
        case 2:  HINT = HINT_INPUT; break;
        case 1:  HINT = HINT_FOCUS; break;
        default: HINT = HINT_IDLE;  break;
    }

    tui_puts_at_padded(row, 1, cols, "");

    const char *p   = HINT;
    const char *end = HINT + strlen(HINT);
    int w = 0;
    int budget = cols;
    while (p < end && w < budget) {
        unsigned char c = (unsigned char)*p;
        int adv = ((c & 0x80) == 0x00) ? 1
                : ((c & 0xE0) == 0xC0) ? 2
                : ((c & 0xF0) == 0xE0) ? 3
                : ((c & 0xF8) == 0xF0) ? 4
                : 1;
        p += adv;
        w += 1;
    }

    tui_goto(row, 1);
    write_str(C_HINT);
    write_str(C_BOLD);
    { ssize_t wr = write(STDOUT_FILENO, HINT, ( unsigned int )(p - HINT)); (void)wr; }
    write_str(C_RESET);
}

/* ================================================================== */
/*  Frame                                                             */
/* ================================================================== */
static void draw_busy_indicator(int row, int cols) {
    tui_puts_at_padded(row, 1, cols, "");

    static const char *const SPIN[] = {
        "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧",
        "⠇", "⠏"
    };
    static const int SPIN_N = 10;

    const char *label = g_cancel_request
        ? "cancelling"
        : "waiting on upstream";

    int label_w = disp_width(label);

    long long t = monotonic_ms();

    int spin_frame = (int)((t / 100) % SPIN_N);

    int cycle  = label_w + 6;
    int travel = (int)((t / 45) % (cycle * 2 + 20));
    int sweep;
    if (travel < 20) {
        sweep = -6;
    } else if (travel < 20 + cycle) {
        sweep = (travel - 20) - 3;
    } else if (travel < 40 + cycle) {
        sweep = label_w + 6;
    } else {
        sweep = label_w + 3 - (travel - 40 - cycle);
    }

    tui_goto(row, 1);
    write_str(C_ACCENT);
    write_str("  ");
    write_str(SPIN[spin_frame]);
    write_str(C_RESET);
    write_str(" ");

    const char *p   = label;
    const char *end = label + strlen(label);
    int col = 0;

    while (p < end) {
        unsigned char c = (unsigned char)*p;
        int adv;
        if      ((c & 0x80) == 0x00) adv = 1;
        else if ((c & 0xE0) == 0xC0) adv = 2;
        else if ((c & 0xF0) == 0xE0) adv = 3;
        else if ((c & 0xF8) == 0xF0) adv = 4;
        else                         adv = 1;

        int in_window = (col >= sweep && col < sweep + 3);
        write_str(in_window ? C_ACCENT : C_MUTED);
        { ssize_t w = write(STDOUT_FILENO, p, (size_t)adv); (void)w; }
        p += adv;
        col += 1;
    }
    write_str(C_RESET);

    long long busy_since;
    pthread_mutex_lock(&g_req_mutex);
    busy_since = g_busy_since;
    pthread_mutex_unlock(&g_req_mutex);

    if (busy_since > 0) {
        long long elapsed = t - busy_since;
        if (elapsed >= 3000) {
            char ebuf[32];
            snprintf(ebuf, sizeof(ebuf), "%llds", elapsed / 1000);
            int ew = (int)strlen(ebuf);
            int ecol = cols - ew;
            if (ecol > col + 4) {
                tui_goto(row, ecol);
                write_str(C_MUTED);
                write_str(ebuf);
                write_str(C_RESET);
            }
        }
    }
}

static int render_frame(const char *input, int input_cursor) {
    int rows = tui_rows();
    int cols = tui_cols();

    if (rows < 8 || cols < 40) return 0;

    /* Begin synchronized update: the terminal buffers everything until
     * the matching end marker, then presents one frame atomically.
     * Ignored harmlessly by terminals that don't implement it. */
    { ssize_t w = write(STDOUT_FILENO, "\x1b[?2026h", 8); (void)w; }

    { ssize_t w = write(STDOUT_FILENO, "\x1b[?25l", 6); (void)w; }

    int status_row    = 1;
    int status_sep    = 2;
    int bottom_rule   = rows;

    int composer_nrows = 0;
    int input_width    = cols - 3;
    if (input_width < 4) input_width = 4;

    wrap_row_t rows_wrap[256];
    int nrows = 0;
    if (g_input_active) {
        nrows = wrap_compute(input, input_width, rows_wrap, 256);
        if (nrows < 1) nrows = 1;
        composer_nrows = input_rows(nrows);
    }

    int composer_rows = composer_nrows;
    int hint_row      = bottom_rule;
    int hint_sep_row  = bottom_rule - 1;
    int composer_top  = composer_rows > 0
                        ? hint_sep_row - composer_rows
                        : hint_sep_row;
    int body_top    = status_sep + 1;
    int body_bot    = composer_rows > 0 ? composer_top - 2
                                         : hint_sep_row - 1;

    int busy_now;
    pthread_mutex_lock(&g_req_mutex);
    busy_now = g_busy;
    pthread_mutex_unlock(&g_req_mutex);

    int spinner_row = -1;
    if (busy_now && body_bot - 1 >= body_top) {
        spinner_row = body_bot;
        body_bot--;
    }

    if (body_bot < body_top) body_bot = body_top;

    g_body_visible = body_bot - body_top + 1;
    if (g_body_visible < 1) g_body_visible = 1;

    draw_status_bar(status_row, cols);

    rule_plain(status_sep, cols);

    for (int r = body_top; r <= body_bot; r++) {
        tui_puts_at_padded(r, 1, cols, "");
    }
    render_transcript(body_top, body_bot, cols);

    if (spinner_row >= 0) {
        draw_busy_indicator(spinner_row, cols);
    }

    if (composer_rows > 0) {
        int sep_row = composer_top - 1;
        if (sep_row > status_sep) rule_plain(sep_row, cols);

        int input_top = composer_top;
        int input_bot = composer_top + composer_rows - 1;
        if (input_bot > hint_sep_row - 1) input_bot = hint_sep_row - 1;

        int cur_row = 0, cur_col = 0;
        wrap_locate(rows_wrap, nrows, input_cursor, &cur_row, &cur_col);

        int window_top = 0;
        if (cur_row >= composer_rows) window_top = cur_row - composer_rows + 1;

        if (input[0] == '\0') {
            tui_goto(input_top, 1);
            emit_all(C_BOLD, C_PROMPT, "> ", C_RESET, NULL);
            const char *ph = g_shell_mode
                ? "Describe what you want done on this host."
                : (g_project.file_count == 0
                    ? "Describe the project to build."
                    : "Ask for a change, or type a question.");
            write_str(C_MUTED);
            tui_puts_at_padded(input_top, 3, cols, ph);
            write_str(C_RESET);
            for (int r = 1; r < composer_rows; r++) {
                if (input_top + r > input_bot) break;
                tui_puts_at_padded(input_top + r, 1, cols, "");
            }
        } else {
            for (int r = 0; r < composer_rows; r++) {
                int row = input_top + r;
                if (row > input_bot) break;

                int src = window_top + r;
                if (r == 0 && src == 0) {
                    tui_goto(row, 1);
                    emit_all(C_BOLD, C_PROMPT, "> ", C_RESET, NULL);
                } else {
                    tui_goto(row, 1);
                    { ssize_t w = write(STDOUT_FILENO, "   ", 3); (void)w; }
                }

                if (src < nrows) {
                    const wrap_row_t *wr = &rows_wrap[src];
                    char buf[INPUT_MAX];
                    int n = wr->len;
                    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
                    memcpy(buf, input + wr->start, n);
                    buf[n] = '\0';
                    tui_puts_at_padded(row, 3, cols, buf);
                } else {
                    tui_puts_at_padded(row, 1, cols, "");
                }
            }
        }
    }

    rule_plain(hint_sep_row, cols);
    {
        int busy;
        pthread_mutex_lock(&g_req_mutex);
        busy = g_busy;
        pthread_mutex_unlock(&g_req_mutex);

        int mode;
        if (busy) {
            mode = 4;
        } else if (g_input_active) {
            mode = 2;
        } else if (g_focus >= 0) {
            pthread_mutex_lock(&g_tr_mutex);
            int is_open_body = 0;
            if (g_focus < g_tr.count) {
                int bw = body_width_for(cols);
                if (g_tr.sections[g_focus].open &&
                    section_is_foldable(&g_tr.sections[g_focus], bw)) {
                    is_open_body = 1;
                }
            }
            pthread_mutex_unlock(&g_tr_mutex);
            mode = is_open_body ? 3 : 1;
        } else {
            mode = 0;
        }
        rule_bottom_with_hint(hint_row, cols, mode);
    }

    if (g_input_active && composer_rows > 0) {
        int input_top = composer_top;
        int input_bot = composer_top + composer_rows - 1;
        if (input_bot > hint_sep_row - 1) input_bot = hint_sep_row - 1;

        int cur_row = 0, cur_col = 0;
        wrap_locate(rows_wrap, nrows, input_cursor, &cur_row, &cur_col);

        int window_top = 0;
        if (cur_row >= composer_rows) window_top = cur_row - composer_rows + 1;

        int cur_screen_row = input_top + (cur_row - window_top);
        int cur_screen_col = 3 + cur_col;
        if (cur_screen_col > cols) cur_screen_col = cols;
        if (cur_screen_row < input_top) cur_screen_row = input_top;
        if (cur_screen_row > input_bot) cur_screen_row = input_bot;

        if (settings_is_open()) {
            settings_render();
            if (settings_is_editing()) {
                ssize_t w = write(STDOUT_FILENO, "\x1b[?25h", 6); (void)w;
            }
        } else {
            tui_goto(cur_screen_row, cur_screen_col);
            ssize_t w = write(STDOUT_FILENO, "\x1b[?25h", 6); (void)w;
        }
    } else {
        if (settings_is_open()) {
            settings_render();
            if (settings_is_editing()) {
                ssize_t w = write(STDOUT_FILENO, "\x1b[?25h", 6); (void)w;
            }
        }
    }

    /* End synchronized update — just before returning 1, after the
     * final cursor placement. */
    { ssize_t w = write(STDOUT_FILENO, "\x1b[?2026l", 8); (void)w; }

    return 1;
}

/* ================================================================== */
/*  Input                                                             */
/* ================================================================== */

static int insert_byte_filtered(char *input, int *len, int *cursor,
                                unsigned char *pending, int *pending_len,
                                unsigned char c) {
    if (*pending_len > 0) {
        if (*pending_len == 1 && c == '[') {
            pending[(*pending_len)++] = c;
            return 0;
        }
        if (*pending_len == 1 && c != '[') {
            *pending_len = 0;
        } else {
            pending[(*pending_len)++] = c;
            if (c >= 0x40 && c <= 0x7E) {
                *pending_len = 0;
            }
            if (*pending_len >= 8) *pending_len = 0;
            return 0;
        }
    }
    if (c == 0x1b) {
        pending[0] = c;
        *pending_len = 1;
        return 0;
    }
    if (*len < INPUT_MAX - 1) {
        memmove(input + *cursor + 1, input + *cursor,
                (size_t)(*len - *cursor + 1));
        input[*cursor] = (char)c;
        (*cursor)++;
        (*len)++;
        return 1;
    }
    return 0;
}

#if defined(_WIN32)
static int consume_paste(char *input, int *len, int *cursor) {
    /* Windows delivers bracketed paste as keystrokes, not as a
     * escape sequence. There is nothing to consume here — the
     * virtual-terminal-input mode in tui.c handles it. */
    (void)input; (void)len; (void)cursor;
    return 0;
}
#else

static int consume_paste(char *input, int *len, int *cursor) {
    int inserted = 0;
    unsigned char window[6] = {0};
    int window_len = 0;

    static const unsigned char END[6] = {0x1b, '[', '2', '0', '1', '~'};

    unsigned char pending[8] = {0};
    int pending_len = 0;

    int last_was_cr = 0;                 /* NEW */

    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, fl & ~O_NONBLOCK);

    for (;;) {
        unsigned char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r != 1) break;

        if (window_len < 6) {
            window[window_len++] = c;
        } else {
            memmove(window, window + 1, 5);
            window[5] = c;
        }

        if (window_len == 6 && memcmp(window, END, 6) == 0) {
            break;
        }

        if (window_len == 6) {
            unsigned char safe = window[0];
            /* NEW: normalize \r and \r\n to a single \n */
            if (safe == '\r') {
                safe = '\n';
                last_was_cr = 1;
            } else if (safe == '\n' && last_was_cr) {
                last_was_cr = 0;
                continue;              /* swallow LF that followed CR */
            } else {
                last_was_cr = 0;
            }
            if (insert_byte_filtered(input, len, cursor,
                                     pending, &pending_len, safe)) {
                inserted++;
            }
        }
    }

    fcntl(STDIN_FILENO, F_SETFL, fl);

    if (window_len > 0 && window_len < 6) {
        for (int i = 0; i < window_len; i++) {
            unsigned char safe = window[i];
            if (safe == '\r') {
                safe = '\n';
                last_was_cr = 1;
            } else if (safe == '\n' && last_was_cr) {
                last_was_cr = 0;
                continue;
            } else {
                last_was_cr = 0;
            }
            if (insert_byte_filtered(input, len, cursor,
                                     pending, &pending_len, safe)) {
                inserted++;
            }
        }
    }

    return inserted;
}

#endif

static void submit_input(char *input, int *len) {
    if (*len == 0) return;
    if (!g_relay_up) {
        tr_push_section(SEC_FAIL, "upstream not ready", "", NULL);
        return;
    }

    g_auto_continue_count = 0;
    g_user_focused = 0;

    pthread_mutex_lock(&g_original_prompt_mutex);
    snprintf(g_original_prompt, sizeof(g_original_prompt), "%s", input);
    g_original_prompt_valid = 1;
    pthread_mutex_unlock(&g_original_prompt_mutex);

    char *copy = strdup(input);
    if (!copy) return;

    pthread_mutex_lock(&g_req_mutex);
    if (g_busy) {
        pthread_mutex_unlock(&g_req_mutex);
        tr_push_section(SEC_PENDING, "busy — wait for the current request",
                        "", NULL);
        free(copy);
        return;
    }
    g_busy = 1;
    g_busy_since = monotonic_ms();
    g_last_error = 0;
    g_cancel_request = 0;
    pthread_mutex_unlock(&g_req_mutex);

    pthread_t tid;
    if (pthread_create(&tid, NULL, request_thread, copy) != 0) {
        tr_push_section(SEC_FAIL, "could not spawn worker", "", NULL);
        free(copy);
        pthread_mutex_lock(&g_req_mutex);
        g_busy = 0;
        g_busy_since = 0;
        pthread_mutex_unlock(&g_req_mutex);
        return;
    }
    pthread_detach(tid);

    input[0] = '\0';
    *len = 0;
}

static int cursor_vertical_move(const char *input, int input_len, int cursor,
                                int width, int delta) {
    if (width < 2) width = 2;
    wrap_row_t rows_wrap[256];
    int nrows = wrap_compute(input, width, rows_wrap, 256);
    if (nrows < 1) return cursor;

    int cur_row, cur_col;
    wrap_locate(rows_wrap, nrows, cursor, &cur_row, &cur_col);

    int target = cur_row + delta;
    if (target < 0) target = 0;
    if (target >= nrows) target = nrows - 1;
    if (target == cur_row) return cursor;

    int col = cur_col;
    if (col > rows_wrap[target].len) col = rows_wrap[target].len;
    int new_off = rows_wrap[target].start + col;
    if (new_off > input_len) new_off = input_len;
    return new_off;
}

/* ================================================================== */
/*  Focus / scroll helpers                                            */
/* ================================================================== */

void scroll_focus_into_view(void) {
    if (g_focus < 0) return;

    int cols = tui_cols();
    int body_width = body_width_for(cols);

    int visible = g_body_visible;
    if (visible < 1) visible = 1;

    int sec_top = 0;
    for (int i = 0; i < g_focus && i < g_tr.count; i++) {
        int was = g_cap_in_focus;
        g_cap_in_focus = 0;
        sec_top += section_height(&g_tr.sections[i], body_width);
        g_cap_in_focus = was;
    }

    int was = g_cap_in_focus;
    g_cap_in_focus = 1;
    int sec_h = section_height(&g_tr.sections[g_focus], body_width);
    g_cap_in_focus = was;

    int scroll = g_tr.scroll;
    int sec_bot = sec_top + sec_h;

    if (sec_h >= visible) {
        g_tr.scroll = sec_top;
    } else if (sec_top < scroll) {
        g_tr.scroll = sec_top;
    } else if (sec_bot > scroll + visible) {
        g_tr.scroll = sec_bot - visible;
    } else {
        return;
    }
    if (g_tr.scroll < 0) g_tr.scroll = 0;
}

static void focus_next(int dir) {
    g_user_focused = 1;

    pthread_mutex_lock(&g_tr_mutex);
    if (g_tr.count == 0) {
        g_focus = -1;
        pthread_mutex_unlock(&g_tr_mutex);
        g_redraw = 1;
        return;
    }

    int i;
    if (g_focus < 0) {
        i = (dir > 0) ? -1 : g_tr.count;
    } else {
        i = g_focus;
    }

    int start = i;
    int found = -1;
    do {
        i += dir;
        if (i < 0) i = g_tr.count - 1;
        if (i >= g_tr.count) i = 0;
        if (section_is_focusable(&g_tr.sections[i])) { found = i; break; }
    } while (i != start);

    g_focus = found;
    if (g_focus >= 0) {
        g_tr.auto_pin = 0;
        scroll_focus_into_view();
    }
    pthread_mutex_unlock(&g_tr_mutex);
    g_redraw = 1;
}

void tr_focus_tail(void) {
    pthread_mutex_lock(&g_tr_mutex);
    int found = -1;
    for (int i = g_tr.count - 1; i >= 0; i--) {
        if (section_is_focusable(&g_tr.sections[i])) { found = i; break; }
    }
    if (found >= 0 && !g_user_focused) {
        g_focus = found;
        g_tr.auto_pin = 1;
    }
    pthread_mutex_unlock(&g_tr_mutex);
    g_redraw = 1;
}

void tr_mark_last_as_diff(void) {
    pthread_mutex_lock(&g_tr_mutex);
    if (g_tr.count > 0) {
        g_tr.sections[g_tr.count - 1].hl_diff = 1;
    }
    pthread_mutex_unlock(&g_tr_mutex);
}

void tr_set_last_hl_path(const char *path) {
    pthread_mutex_lock(&g_tr_mutex);
    if (g_tr.count > 0) {
        snprintf(g_tr.sections[g_tr.count - 1].hl_path,
                 sizeof(g_tr.sections[g_tr.count - 1].hl_path),
                 "%s", path ? path : "");
    }
    pthread_mutex_unlock(&g_tr_mutex);
}

static int toggle_focused(void) {
    if (g_focus < 0) return 0;

    g_user_focused = 1;

    int cols = tui_cols();
    int body_width = body_width_for(cols);

    int toggled = 0;
    int opened  = 0;
    pthread_mutex_lock(&g_tr_mutex);
    if (g_focus < g_tr.count) {
        section_t *s = &g_tr.sections[g_focus];
        if (section_is_foldable(s, body_width)) {
            s->open = !s->open;
            s->scroll = 0;
            opened = s->open;
            toggled = 1;
            if (opened) {
                scroll_focus_into_view();
            }
        }
    }
    pthread_mutex_unlock(&g_tr_mutex);

    if (toggled) g_redraw = 1;
    return toggled;
}

static void esc_focus(void) {
    int had_focus = (g_focus >= 0);

    if (had_focus) {
        g_user_focused = 1;

        int cols = tui_cols();
        int body_width = body_width_for(cols);

        pthread_mutex_lock(&g_tr_mutex);
        if (g_focus < g_tr.count) {
            section_t *s = &g_tr.sections[g_focus];
            if (s->open && section_is_foldable(s, body_width)) {
                s->open = 0;
                s->scroll = 0;
            }
        }
        g_focus = -1;
        pthread_mutex_unlock(&g_tr_mutex);
    } else {
        g_user_focused = 0;
        g_tr.auto_pin = 1;
    }
    g_redraw = 1;
}

static int arrow_scroll_body(int delta) {
    wisp_logf("arrow | delta=%d focus=%d", delta, g_focus);
    if (g_focus < 0) return 0;

    int cols = tui_cols();
    int body_width = body_width_for(cols);

    int consumed = 0;
    pthread_mutex_lock(&g_tr_mutex);
    if (g_focus < g_tr.count) {
        section_t *s = &g_tr.sections[g_focus];
        if (s->open && section_is_foldable(s, body_width)) {
            int was = g_cap_in_focus;
            g_cap_in_focus = 1;
            int total = section_body_rows(s, body_width);
            int cap   = body_visible_cap(total);
            g_cap_in_focus = was;

            int max   = total > cap ? total - cap : 0;
            int next  = s->scroll + delta;
            if (next < 0) next = 0;
            if (next > max) next = max;
            wisp_logf("arrow | total=%d cap=%d max=%d scroll=%d next=%d",
                      total, cap, max, s->scroll, next);
            if (next != s->scroll) {
                s->scroll = next;
                consumed = 1;
            }
        }
    }
    pthread_mutex_unlock(&g_tr_mutex);

    if (consumed) g_redraw = 1;
    return consumed;
}

static int page_scroll_body(int dir) {
    if (g_focus < 0) return 0;

    int cols = tui_cols();
    int body_width = body_width_for(cols);

    int consumed = 0;
    pthread_mutex_lock(&g_tr_mutex);
    if (g_focus < g_tr.count) {
        section_t *s = &g_tr.sections[g_focus];
        if (s->open && section_is_foldable(s, body_width)) {
            int was = g_cap_in_focus;
            g_cap_in_focus = 1;
            int total = section_body_rows(s, body_width);
            int cap   = body_visible_cap(total);
            g_cap_in_focus = was;

            int max   = total > cap ? total - cap : 0;
            int step  = cap > 1 ? cap - 1 : 1;
            int next  = s->scroll + dir * step;
            if (next < 0) next = 0;
            if (next > max) next = max;
            if (next != s->scroll) {
                s->scroll = next;
                consumed = 1;
            }
        }
    }
    pthread_mutex_unlock(&g_tr_mutex);

    if (consumed) g_redraw = 1;
    return consumed;
}

static void scroll_transcript(int delta) {
    g_tr.auto_pin = 0;
     g_user_focused = 1;

    pthread_mutex_lock(&g_tr_mutex);
    int cols = tui_cols();
    int body_width = body_width_for(cols);

    int total = 0;
    for (int i = 0; i < g_tr.count; i++) {
        int was = g_cap_in_focus;
        g_cap_in_focus = (g_focus == i);
        total += section_height(&g_tr.sections[i], body_width);
        g_cap_in_focus = was;
    }

    int visible = g_body_visible;
    if (visible < 1) visible = 1;

    int max_off = total > visible ? total - visible : 0;
    int next = g_tr.scroll + delta;
    if (next < 0) next = 0;
    if (next > max_off) next = max_off;
    g_tr.scroll = next;
    pthread_mutex_unlock(&g_tr_mutex);
    g_redraw = 1;
}

/* ================================================================== */
/*  Key handling                                                      */
/* ================================================================== */

static int handle_key(int ch, char *input, int *len, int *cursor) {
    int cols = tui_cols();
    int width = cols - 3;
    if (width < 4) width = 4;

    if (settings_is_open()) {
        if (ch == 3) return 1;
        if (settings_handle_key(ch)) {
            g_redraw = 1;
        }
        return 0;
    }

    switch (ch) {
        case 3:
            wisp_logf("key  | Ctrl-C");
            return 1;

        case 12:
            wisp_logf("key  | Ctrl-L");
            g_redraw = 1;
            return 0;

        case 9:
            wisp_logf("key  | Tab, focus=%d", g_focus);
            focus_next(1);
            return 0;

        case 10:
            wisp_logf("key  | Ctrl-J");
            g_input_active = 1;
            if (*len < INPUT_MAX - 1) {
                memmove(input + *cursor + 1, input + *cursor,
                        (size_t)(*len - *cursor + 1));
                input[*cursor] = '\n';
                (*cursor)++;
                (*len)++;
            }
            g_redraw = 1;
            return 0;

        case 13:
            if (g_input_active && *len > 0) {
                int busy;
                pthread_mutex_lock(&g_req_mutex);
                busy = g_busy;
                pthread_mutex_unlock(&g_req_mutex);

                if (busy) {
                    char *copy = strdup(input);
                    if (copy) {
                        request_queue_prompt(copy);
                        request_cancel();
                        tr_push_section(SEC_PENDING,
                            "queued — cancelling current request",
                            "", NULL);
                    }
                    input[0] = '\0';
                    *len = 0;
                    *cursor = 0;
                    g_input_active = 0;
                    g_redraw = 1;
                    return 0;
                }

                submit_input(input, len);
                *cursor = 0;
                g_input_active = 0;
                g_redraw = 1;
                return 0;
            }
            if (!g_input_active) {
                if (g_focus >= 0) {
                    toggle_focused();
                } else {
                    g_input_active = 1;
                    g_redraw = 1;
                }
            }
            g_redraw = 1;
            return 0;

        case 127:
        case 8:
            if (*cursor > 0) {
                memmove(input + *cursor - 1, input + *cursor,
                        (size_t)(*len - *cursor + 1));
                (*cursor)--;
                (*len)--;
            }
            g_redraw = 1;
            return 0;

        case 27: {
            int a = tui_try_getch();
            wisp_logf("key  | ESC seq: a=%d", a);
            if (a < 0) {
                int busy;
                pthread_mutex_lock(&g_req_mutex);
                busy = g_busy;
                pthread_mutex_unlock(&g_req_mutex);

                if (busy && *len == 0) {
                    if (request_cancel()) {
                        g_redraw = 1;
                    }
                    return 0;
                }

                if (g_input_active) {
                    g_input_active = 0;
                    input[0] = '\0';
                    *len = 0;
                    *cursor = 0;
                    wisp_logf("key  | Esc (close input)");
                    g_redraw = 1;
                    return 0;
                }
                wisp_logf("key  | Esc (clear focus / pin)");
                esc_focus();
                return 0;
            }

            if (a == 'b' || a == 'B') {
                wisp_logf("key  | Alt-B, run");
                start_build();
                g_redraw = 1;
                return 0;
            }

            if (a == 't' || a == 'T') {
                int idx = theme_next();
                const theme_t *t = theme_at(idx);
                wisp_logf("key  | Alt-T, theme -> %s", t->name);
                theme_save(g_project.root);
                g_redraw = 1;
                return 0;
            }

            if (a == 's' || a == 'S') {
                wisp_logf("key  | Alt-S, settings");
                settings_open();
                g_redraw = 1;
                return 0;
            }

            if (a == '\r' || a == '\n') {
                g_input_active = 1;
                if (*len < INPUT_MAX - 1) {
                    memmove(input + *cursor + 1, input + *cursor,
                            (size_t)(*len - *cursor + 1));
                    input[*cursor] = '\n';
                    (*cursor)++;
                    (*len)++;
                }
                g_redraw = 1;
                return 0;
            }

            int b = tui_try_getch();
            if (b < 0) return 0;

            if (b == '2') {
                int c1 = tui_try_getch();
                if (c1 == '0') {
                    int c2 = tui_try_getch();
                    int c3 = tui_try_getch();
                    if (c2 == '0' && c3 == '~') {
                        g_input_active = 1;
                        int n = consume_paste(input, len, cursor);
                        wisp_logf("key  | pasted %d bytes", n);
                        g_redraw = 1;
                    }
                    return 0;
                }
                if (c1 == '7') {
                    int semi = tui_try_getch();
                    if (semi != ';') return 0;
                    int mod = 0;
                    int c;
                    while ((c = tui_try_getch()) >= '0' && c <= '9') {
                        mod = mod * 10 + (c - '0');
                    }
                    if (c != ';') return 0;
                    int key = 0;
                    while ((c = tui_try_getch()) >= '0' && c <= '9') {
                        key = key * 10 + (c - '0');
                    }
                    if (c != '~') return 0;

                    if (mod == 2 && key == 13) {
                        g_input_active = 1;
                        if (*len < INPUT_MAX - 1) {
                            memmove(input + *cursor + 1, input + *cursor,
                                    (size_t)(*len - *cursor + 1));
                            input[*cursor] = '\n';
                            (*cursor)++;
                            (*len)++;
                        }
                        g_redraw = 1;
                    }
                    return 0;
                }
                return 0;
            }

            switch (b) {
                case 'A':
                    if (arrow_scroll_body(-1)) return 0;
                    *cursor = cursor_vertical_move(input, *len, *cursor,
                                                   width, -1);
                    break;
                case 'B':
                    if (arrow_scroll_body(+1)) return 0;
                    *cursor = cursor_vertical_move(input, *len, *cursor,
                                                   width, +1);
                    break;
                case 'C':
                    if (*cursor < *len) (*cursor)++;
                    break;
                case 'D':
                    if (*cursor > 0) (*cursor)--;
                    break;

                case 'Z':
                    wisp_logf("key  | Shift+Tab, focus=%d", g_focus);
                    focus_next(-1);
                    break;

                case 'H': *cursor = 0; break;
                case 'F': *cursor = *len; break;

                case '1': {
                    int n1 = tui_try_getch();
                    if (n1 == '~') {
                        *cursor = 0;
                        break;
                    }
                    if (n1 == '3') {
                        int n2 = tui_try_getch();
                        if (n2 != ';') break;
                        int n3 = tui_try_getch();
                        int term = tui_try_getch();
                        if (term == 'u' || term == '~') {
                            if (n3 == '2') {
                                g_input_active = 1;
                                if (*len < INPUT_MAX - 1) {
                                    memmove(input + *cursor + 1,
                                            input + *cursor,
                                            (size_t)(*len - *cursor + 1));
                                    input[*cursor] = '\n';
                                    (*cursor)++;
                                    (*len)++;
                                }
                                g_redraw = 1;
                            }
                        }
                        break;
                    }
                    *cursor = 0;
                    break;
                }
                case '3': {
                    int tilde = tui_try_getch(); (void)tilde;
                    if (*cursor < *len) {
                        memmove(input + *cursor, input + *cursor + 1,
                                (size_t)(*len - *cursor));
                        (*len)--;
                    }
                    break;
                }
                case '4': {
                    int tilde = tui_try_getch(); (void)tilde;
                    *cursor = *len;
                    break;
                }
                case '5': {
                    int tilde = tui_try_getch(); (void)tilde;
                    if (!page_scroll_body(-1)) scroll_transcript(-5);
                    break;
                }
                case '6': {
                    int tilde = tui_try_getch(); (void)tilde;
                    if (!page_scroll_body(+1)) scroll_transcript(+5);
                    break;
                }
                default:
                    break;
            }
            g_redraw = 1;
            return 0;
        }

        case 1: *cursor = 0; g_redraw = 1; return 0;
        case 4:
            if (*cursor < *len) {
                memmove(input + *cursor, input + *cursor + 1,
                        (size_t)(*len - *cursor));
                (*len)--;
            }
            g_redraw = 1; return 0;
        case 5: *cursor = *len; g_redraw = 1; return 0;

        case 11:
            input[*cursor] = '\0';
            *len = *cursor;
            g_redraw = 1;
            return 0;

        case 21:
            input[0] = '\0';
            *len = 0;
            *cursor = 0;
            g_redraw = 1;
            return 0;

        case 23: {
            int i = *cursor;
            while (i > 0 && (input[i-1] == ' ' || input[i-1] == '\t')) i--;
            while (i > 0 && input[i-1] != ' ' && input[i-1] != '\t' &&
                   input[i-1] != '\n') i--;
            if (i < *cursor) {
                memmove(input + i, input + *cursor,
                        (size_t)(*len - *cursor + 1));
                *len -= (*cursor - i);
                *cursor = i;
            }
            g_redraw = 1;
            return 0;
        }

        default:
            if (ch >= 0x20 && ch < 0x7f && *len < INPUT_MAX - 1) {
                g_input_active = 1;
                memmove(input + *cursor + 1, input + *cursor,
                        (size_t)(*len - *cursor + 1));
                input[*cursor] = (char)ch;
                (*cursor)++;
                (*len)++;
                g_redraw = 1;
            }
            return 0;
    }
}

/* ================================================================== */
/*  Project-root detection                                            */
/* ================================================================== */

static int looks_like_project(const char *dir) {
    char path[MAX_PATH_LEN * 2];
    struct stat st;

    snprintf(path, sizeof(path), "%s/wisp-code.conf", dir);
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) return 1;

    snprintf(path, sizeof(path), "%s/.git", dir);
    if (stat(path, &st) == 0) return 1;

    snprintf(path, sizeof(path), "%s/operator.md", dir);
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) return 1;

    snprintf(path, sizeof(path), "%s/rules.md", dir);
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) return 1;

    static const char *markers[] = {
        "CMakeLists.txt", "Makefile", "Cargo.toml", "go.mod",
        "package.json", "pyproject.toml", "setup.py", "composer.json",
        "Gemfile", "build.gradle", "build.gradle.kts", "pom.xml",
        "build.zig", "Package.swift", "Dockerfile",
        NULL
    };
    for (int i = 0; markers[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, markers[i]);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) return 1;
    }
    return 0;
}

static char *load_md_file(const char *root, const char *name,
                          const char *label) {
    if (!name || !name[0]) return NULL;

    char path[MAX_PATH_LEN * 2 + 2];
    snprintf(path, sizeof(path), "%s/%s", root, name);

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    if (sz > (1024 * 1024)) {
        fclose(f);
        char buf[256];
        snprintf(buf, sizeof(buf), "%s file too large: %s", label, name);
        tr_push_section(SEC_PENDING, buf, "", NULL);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* ================================================================== */
/*  main                                                              */
/* ================================================================== */

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--project <dir>] [-shell]\n"
        "\n"
        "  --project <dir>   load this folder (created if missing).\n"
        "  -shell            shell mode.\n"
        "\n"
        "console keys:\n"
        "  Enter     send input (when open), toggle fold (when focused).\n"
        "  Esc       close the input, collapse + unfocus a section, or\n"
        "            pin the transcript to the bottom.\n"
        "  Tab       focus the next section.\n"
        "  Shift+Tab focus the previous section.\n"
        "  Arrows    move cursor in input; scroll an open body; else\n"
        "            scroll the transcript.\n"
        "  PgUp/PgDn scroll an open body, else the transcript.\n"
        "  Alt-B     run the configured command.\n"
        "  Alt-T     cycle theme.\n"
        "  Alt-S     open settings.\n"
        "  Ctrl-J    newline within input.\n"
        "  Ctrl-L    force a redraw.\n"
        "  Ctrl-C    quit.\n"
        "\n"
        "the input is hidden until you start typing.\n",
        argv0);
}

static void on_signal_cleanup(int sig) {
    (void)sig;
    tui_shutdown();
    _exit(0);
}

static void install_signal_cleanup(void) {
    signal(SIGTERM, on_signal_cleanup);
    signal(SIGINT,  on_signal_cleanup);
    signal(SIGHUP,  on_signal_cleanup);
    signal(SIGQUIT, on_signal_cleanup);
}

static void canonicalize_root(const char *in, char *out, size_t out_sz) {
    if (!in || !in[0] || !out || out_sz < 2) {
        if (out && out_sz) out[0] = '\0';
        return;
    }
#ifdef _WIN32
    char resolved[_MAX_PATH];
    if (_fullpath(resolved, in, sizeof(resolved))) {
        snprintf(out, out_sz, "%s", resolved);
    } else {
        snprintf(out, out_sz, "%s", in);
    }
#else
    char resolved[PATH_MAX];
    if (realpath(in, resolved)) {
        snprintf(out, out_sz, "%s", resolved);
    } else {
        snprintf(out, out_sz, "%s", in);
    }
#endif
}

int main(int argc, char **argv) {
    g_project.root[0] = '\0';

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            snprintf(g_project.root, sizeof(g_project.root), "%s", argv[++i]);
        } else if (strcmp(argv[i], "-shell") == 0) {
            g_shell_mode = 1;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    theme_init();

    if (g_project.root[0] == '\0') {
        char cwd[MAX_PATH_LEN];
        if (!getcwd(cwd, sizeof(cwd))) {
            fprintf(stderr, "wisp-code: cannot determine cwd: %s\n",
                    strerror(errno));
            return 1;
        }
        snprintf(g_project.root, sizeof(g_project.root), "%s", cwd);
        fprintf(stderr, "wisp-code: using %s\n", cwd);
    }

    size_t rlen = strlen(g_project.root);
    while (rlen > 1 && g_project.root[rlen - 1] == '/') {
        g_project.root[--rlen] = '\0';
    }

    {
        char canon[MAX_PATH_LEN];
        canonicalize_root(g_project.root, canon, sizeof(canon));
        snprintf(g_project.root, sizeof(g_project.root), "%s", canon);
    }

    struct stat st;
    if (stat(g_project.root, &st) != 0) {
        if (mkdir(g_project.root, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "wisp-code: cannot create %s: %s\n",
                    g_project.root, strerror(errno));
            return 1;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "wisp-code: %s exists but is not a directory\n",
                g_project.root);
        return 1;
    }

    theme_load(g_project.root);
    settings_load(g_project.root);
    run_conf_get(g_project.root, "run", g_build_run, sizeof(g_build_run));

    if (!g_shell_mode && g_project.root[0]) {
        if (!looks_like_project(g_project.root)) {
            fprintf(stderr, "wisp-code: %s looks empty — bootstrap mode\n",
                    g_project.root);
        }
    }

    if (!g_shell_mode) {
        if (build_snapshot() != 0) {
            fprintf(stderr, "wisp-code: snapshot build failed\n");
            return 1;
        }
    }

    {
        char env[512];
        platform_describe(env, sizeof(env));

        const char *rules_name = settings_rules_file();
        if (!rules_name || !rules_name[0]) rules_name = "rules.md";

        const char *ops_name = settings_operator_file();
        if (!ops_name || !ops_name[0]) ops_name = "operator.md";

        char *rules = load_md_file(g_project.root, rules_name, "rules");
        char *ops   = load_md_file(g_project.root, ops_name, "operator");

        const char *rules_hdr = "--- PROJECT RULES ---\n";
        const char *rules_ftr = "\n--- END PROJECT RULES ---\n";
        const char *ops_hdr   = "\n--- OPERATOR INSTRUCTIONS ---\n";
        const char *ops_ftr   = "\n--- END OPERATOR INSTRUCTIONS ---\n";

        size_t rules_hdr_len = rules ? strlen(rules_hdr) : 0;
        size_t rules_ftr_len = rules ? strlen(rules_ftr) : 0;
        size_t rules_len     = rules ? strlen(rules) : 0;
        size_t ops_hdr_len   = ops   ? strlen(ops_hdr)   : 0;
        size_t ops_ftr_len   = ops   ? strlen(ops_ftr)   : 0;
        size_t ops_len       = ops   ? strlen(ops)       : 0;
        size_t shell_len     = g_shell_mode ? strlen(SHELL_MODE_FRAGMENT) : 0;

        size_t sys_len = strlen(SYSTEM_PROMPT);
        size_t env_len = strlen(env);
        size_t need = sys_len + env_len + shell_len
                    + rules_hdr_len + rules_ftr_len + rules_len
                    + ops_hdr_len   + ops_ftr_len   + ops_len
                    + 64;

        char *prefix = malloc(need);
        if (prefix) {
            int n = snprintf(prefix, need,
                "%s\n\n"
                "%s%s%s"
                "%s%s%s"
                "%s"
                "%s\n",
                SYSTEM_PROMPT,
                rules ? rules_hdr : "",
                rules ? rules     : "",
                rules ? rules_ftr : "",
                ops   ? ops_hdr   : "",
                ops   ? ops       : "",
                ops   ? ops_ftr   : "",
                g_shell_mode ? SHELL_MODE_FRAGMENT : "",
                env);
            if (n > 0 && (size_t)n < need) {
                history_init(prefix);
            }
            free(prefix);
        }
        free(rules);
        free(ops);
    }

    wisp_log_open(g_project.root);
    wisp_logf("main: log opened");
    wisp_logf("main: theme '%s'", theme_current()->name);
    wisp_logf("main: mode '%s'", g_shell_mode ? "shell" : "code");

    install_signal_cleanup();

    if (tui_init() != 0) {
        fprintf(stderr, "wisp-code: could not enter raw mode (not a tty?)\n");
        return 1;
    }

    {
        char buf[MAX_PATH_LEN + 128];
        snprintf(buf, sizeof(buf), "project %s (%d files)",
                 g_project.root, g_project.file_count);
        tr_push_section(SEC_PLAIN, buf, "", NULL);
    }
    g_relay_up = 1;

    char input[INPUT_MAX] = {0};
    int  len = 0;
    int  cursor = 0;

    long long last_size_check = monotonic_ms();
    int last_rows = tui_rows();
    int last_cols = tui_cols();

    while (!g_quit) {
        int ch = tui_try_getch();
        if (ch >= 0) {
            if (handle_key(ch, input, &len, &cursor)) break;
        } else {
            usleep(15000);
        }

        long long now = monotonic_ms();

        if (tui_check_resize()) {
            tui_clear();
            g_redraw = 1;
            last_rows = tui_rows();
            last_cols = tui_cols();
            last_size_check = now;
        }

        if (now - last_size_check >= 1000) {
            last_size_check = now;
            int r = tui_rows();
            int c = tui_cols();
            if (r != last_rows || c != last_cols) {
                last_rows = r;
                last_cols = c;
                tui_clear();
                g_redraw = 1;
            }
        }

        {
            int busy;
            pthread_mutex_lock(&g_req_mutex);
            busy = g_busy;
            pthread_mutex_unlock(&g_req_mutex);
            if (busy && !g_redraw) {
                static long long last_busy_tick = 0;
                if (now - last_busy_tick >= 150) {
                    last_busy_tick = now;
                    g_redraw = 1;
                }
            }
        }

        if (g_redraw ) {
            /* Clear before rendering, not after. If a worker thread pushes a
            * section while render_frame is running, its g_redraw=1 set lands
            * on a value of 0 and survives the render; the next loop iteration
            * picks it up. Clearing after the render was the classic lost-
            * wakeup: the worker's set was wiped by render's "done" write. */
            g_redraw = 0;
            if (!render_frame(input, cursor)) {
                g_redraw = 1;   /* render failed (pty busy), try again next loop */
            }
        }
    }

    tui_shutdown();

    wisp_logf("main: shutting down");
    wisp_log_close();

    free(g_project.snapshot);
    free(g_pending_run_output);
    g_pending_run_output = NULL;

    pthread_mutex_lock(&g_tr_mutex);
    for (int i = 0; i < g_tr.count; i++) section_free(&g_tr.sections[i]);
    pthread_mutex_unlock(&g_tr_mutex);

    printf("wisp-code exited.\n");
    return 0;
}