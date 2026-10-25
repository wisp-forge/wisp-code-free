// language: C, file: wisp-code/console.h
#ifndef CONSOLE_H
#define CONSOLE_H

#include "compat/posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef _WIN32
    #include "compat/threads.h"
#else

    #include <pthread.h>
#endif

#include <sys/types.h>

#include "mongoose.h"
#include "cJSON.h"

/* ================================================================== */
/*  Config                                                            */
/* ================================================================== */
#define HISTORY_MAX_BYTES   (1024 * 1024)  /* 1 MiB history[] byte cap   */
#define HISTORY_MAX_ENTRIES 128            /* history[] entry cap        */

/* Approximate chars-per-token used for the status bar's token
 * estimate. 4 is right for English prose; code skews lower
 * (2.5–3.5). We round to 4 because the status bar is a
 * gestalt indicator, not a billing meter. */
#define CHARS_PER_TOKEN     4

#define MAX_PATH_LEN      512
#define MAX_FILE_BYTES    (4 * 1024 * 1024)
#define MAX_TREE_BYTES    (48 * 1024 * 1024)
#define MAX_FILES         8192
#define MAX_SECTIONS      2048          /* rows in the single transcript */
#define MAX_BODY_BYTES    (256 * 1024)  /* cap on a section body */
#define MAX_CODE_LINES    8192

#define CODE_SECTION_MAX_BYTES (256 * 1024)
#define CODE_SECTION_WINDOW    200   /* ±100 lines around the jump */

#define INPUT_MAX (64 * 1024)   /* was 1024-ish; paste-friendly */

#define WISP_HTTP_REFERER ""
#define WISP_X_TITLE  "wisp-code"


#ifdef _WIN32
  #include <process.h>
  #define wisp_getpid() ((long)_getpid())
#else
  #include <unistd.h>
  #define wisp_getpid() ((long)getpid())
#endif

/* ================================================================== */
/*  Project model                                                     */
/* ======================== */

typedef struct {
    char   path[MAX_PATH_LEN];
    size_t size;
    time_t mtime;
} file_entry;

typedef struct {
    file_entry files[MAX_FILES];
    int        count;
} file_list_t;

typedef struct {
    char        root[MAX_PATH_LEN];
    file_entry  files[MAX_FILES];
    int         file_count;
    char       *snapshot;
    size_t      snapshot_len;
    file_list_t last_sent;
    int         last_sent_valid;
} project_t;

extern project_t g_project;
extern int g_last_reply_bytes;

/* ================================================================== */
/*  Shared flags                                                      */
/* ================================================================== */

extern volatile int g_quit;
extern int          g_last_error;
extern int          g_shell_mode;
extern char         g_build_run[1024];
extern char        *g_pending_run_output;

/* ================================================================== */
/*  Sections                                                          */
/*                                                                    */
/*  The transcript is a single FIFO of sections, stacked top-to-      */
/*  bottom, exactly like the browser mockup. Each section has a       */
/*  state pill, a fold indicator, a short label, an optional meta     */
/*  on the right, and an optional body that expands under the head.   */
/*                                                                    */
/*  The SEC_BANNER_* and SEC_THEME kinds are chrome-less: no rail,    */
/*  no pill, no fold, no meta. They render through the same code      */
/*  path as SEC_PLAIN; the only difference is the color returned by   */
/*  color_for_kind().                                                 */
/* ================================================================== */

typedef enum {
    SEC_DONE    = 0,   /* [DONE]     — completed successfully */
    SEC_PENDING = 1,   /* [PENDING]  — queued or in progress */
    SEC_FAIL    = 2,   /* [FAILED]   — operation failed */
    SEC_IDLE    = 3,   /* [IDLE]     — not yet started */
    SEC_SAYS    = 4,   /* [SAYS]     — assistant prose */
    SEC_USER    = 5,   /* user prompt — "> " prefix, no pill */
    SEC_PLAIN   = 6,   /* untagged info — no pill, no rail */
    SEC_BANNER_WORD  = 7,   /* splash wordmark — accent, no chrome */
    SEC_BANNER_CODE  = 8,   /* splash -code-   — title,  no chrome */
    SEC_BANNER_SPARK = 9,   /* splash sparkles — muted,  no chrome */
    SEC_THEME        = 10,  /* "theme: <name>" — accent, no chrome */
    SEC_CODE         = 11,  /* [CODE] — %%%JUMP target, file window */
    SEC_READ         = 12,  /* [READ] — %%%READ target, whole file  */
    SEC_RUN          = 13   /* [RUN] */
} sec_kind_t;

typedef struct {
    sec_kind_t kind;
    char      *label;     /* was char label[N] */
    char       meta[64];
    char      *body;
    size_t     body_len;
    int        open;
    int        scroll;
    int        hl_diff;
    char       hl_path[MAX_PATH_LEN];
} section_t;

/* ================================================================== */
/*  Console output — single transcript (console.c)                    */
/*                                                                    */
/*  All pushes append to the one and only transcript. The kind        */
/*  decides the pill, the rail colour, and the head marker.           */
/* ================================================================== */

void tr_push_user(const char *line);
void tr_push_userf(const char *fmt, ...);

void tr_push_system(const char *line);
void tr_push_systemf(const char *fmt, ...);

void tr_push_error(const char *line);
void tr_push_errorf(const char *fmt, ...);

void tr_push_assistant(const char *line);
void tr_push_assistantf(const char *fmt, ...);
void tr_push_block_assistant(const char *s);

/* Legacy shims. */
void tr_push(const char *line);
void tr_pushf(const char *fmt, ...);
void tr_push_block(const char *s);

/* Section-level push. */
void tr_push_section(sec_kind_t kind, const char *label,
                     const char *meta, const char *body);
void tr_push_sectionf(sec_kind_t kind, const char *label,
                      const char *meta, const char *body_fmt, ...);

void tr_focus_tail(void);

/* Mark the most recently pushed section as a diff body. The
 * renderer will use the diff highlighter instead of the syntax
 * highlighter. No-op if the transcript is empty. */
void tr_mark_last_as_diff(void);

/* Set the language-detection path for the most recently pushed
 * section. Called by tr_push_code_jump / tr_push_read_file right
 * after the push. No-op if the transcript is empty. */
void tr_set_last_hl_path(const char *path);

/* ================================================================== */
/*  Ops push — alias for tr_push_section                              */
/*                                                                    */
/*  Historically a separate panel; now the same transcript. Callers   */
/*  in reply.c and project.c still use these names, so they stay.     */
/* ================================================================== */

void ops_push(const char *line);
void ops_pushf(const char *fmt, ...);

void ops_push_section(sec_kind_t kind, const char *label,
                      const char *meta, const char *body);
void ops_push_sectionf(sec_kind_t kind, const char *label,
                       const char *meta, const char *body_fmt, ...);

void ops_push_kind(int kind, const char *line);
void ops_push_kindf(int kind, const char *fmt, ...);

/* ================================================================== */
/*  Code view — stub                                                  */
/* ================================================================== */

void code_clear(void);
void code_set(const char *label, const char *text);
int  code_load_file(const char *rel);
void code_jump_to(const char *rel_path, int line);

/* Push a SEC_CODE section showing `rel_path` around `line`. The body
 * is the whole file if it fits under CODE_SECTION_MAX_BYTES, else a
 * ±100 line window capped at the same size. `line` is 1-based; the
 * section's initial scroll positions `line` at the top of the body
 * viewport. Safe to call from a worker thread. */
void tr_push_code_jump(const char *rel_path, int line);

/* Push a SEC_READ section showing the whole file body. Meta is the
 * byte count. Body is bounded by MAX_BODY_BYTES. */
void tr_push_read_file(const char *rel_path);

/* ================================================================== */
/*  Project (project.c)                                               */
/* ================================================================== */

int  build_snapshot(void);
int  write_one_file(const char *rel, const char *body, size_t body_len);

int  path_is_safe(const char *rel);
int  path_is_safe_read(const char *p);

int  conf_get(const char *root, const char *key, char *out, size_t out_sz);
int  run_conf_get(const char *root, const char *key, char *out, size_t out_sz);

void snapshot_capture_baseline(void);
void snapshot_emit_delta(char *buf, size_t buf_sz);
void snapshot_emit_reads(char *buf, size_t buf_sz);

void read_cache_add(const char *rel);
int  read_cache_contains(const char *rel);
void read_cache_clear(void);

/* ================================================================== */
/*  Conversation history                                              */
/* ================================================================== */

void history_init(const char *system_prefix);
void history_append_user(const char *content);
void history_append_assistant(const char *content);
void history_clear(void);

struct cJSON *history_to_json(void);
struct cJSON *history_to_json_pair(void);
struct cJSON *history_to_json_last(void);

/* Sum of strlen() over history[1..count-1]. The system prefix at
 * index 0 is not included — see history_total_bytes() for the
 * figure that matches what actually leaves the machine. */
size_t history_bytes(void);

/* history_bytes() plus the system prefix at index 0. This is the
 * number the status bar reports, because it is what the upstream
 * sees in the request body (modulo JSON escaping). */
size_t history_total_bytes(void);

/* System prefix length alone. Exposed so callers can compute
 * "conversation only" vs "total" without taking the lock twice. */
size_t history_system_len(void);


/* ================================================================== */
/*  HTTP                                                              */
/* ================================================================== */

typedef struct {
    char  *body;
    size_t len;
    int    status;
} http_reply_t;

char *http_get_text(const char *url, const char *bearer_token,
                    char *err_out, size_t err_sz);

extern volatile int g_cancel_request;                    
int  http_post_json(const char *body, http_reply_t *out, int timeout_ms);
int  tcp_port_open(const char *host, int port, int timeout_ms);

char *build_request_body(const char *user_prompt);
char *extract_content(const char *reply_json);

/* ================================================================== */
/*  Reply pipeline                                                    */
/* ================================================================== */

int  run_files_from_markers(const char *reply, int *written_out);
int  process_reply(const char *content);

/* ================================================================== */
/*  Log                                                               */
/* ================================================================== */

void  wisp_log_open(const char *project_root);
void  wisp_log_close(void);
void  wisp_logf(const char *fmt, ...);
void  wisp_log_raw(const void *buf, size_t len);


/* ================================================================== */
/*  Macros (macro.c)                                                  */
/* ================================================================== */

/* Run slot `n` (1..MACRO_SLOTS) — Alt-1 through Alt-9. If the slot
 * is empty, this is a silent no-op. Otherwise it dispatches through
 * the same pipeline as Alt-B: run_capture, [RUN] section, and
 * g_pending_run_output for the follow-up. */
void start_macro(int n);

/* Parse %%%MACRO blocks from a reply. Returns the number of slots
 * set or cleared. Duplicate slots within one reply are rejected:
 * the first definition wins, the second is reported as [FAILED]. */
int run_macros_from_reply(const char *reply);

#endif /* CONSOLE_H */