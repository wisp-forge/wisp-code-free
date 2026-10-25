// language: C, file: wisp-code/project.c
#include "console.h"

#if defined(_WIN32)
  #include <windows.h>
#endif

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>


project_t g_project;


/* ================================================================== */
/*  Read cache                                                        */
/*                                                                    */
/*  When the model emits a %%%READ block for a path, it is recorded   */
/*  here. build_snapshot() checks this list before falling back to    */
/*  the baseline placeholder logic, and inlines any cached path in    */
/*  full. The cache is cleared after a request is composed, so each   */
/*  read only costs one turn.                                         */
/* ================================================================== */
int  path_is_safe(const char *rel);        /* forward decls */
int  path_is_safe_read(const char *p);

#define READ_CACHE_MAX 64

static char            g_read_paths[READ_CACHE_MAX][MAX_PATH_LEN];
static int             g_read_count = 0;
static pthread_mutex_t g_read_mutex = PTHREAD_MUTEX_INITIALIZER;

void read_cache_add(const char *rel) {
    if (!rel || !rel[0]) return;
    if (path_is_safe_read(rel) != 0) return;

    pthread_mutex_lock(&g_read_mutex);
    if (g_read_count < READ_CACHE_MAX) {
        int already = 0;
        for (int i = 0; i < g_read_count; i++) {
            if (strcmp(g_read_paths[i], rel) == 0) { already = 1; break; }
        }
        if (!already) {
            snprintf(g_read_paths[g_read_count], MAX_PATH_LEN, "%s", rel);
            g_read_count++;
        }
    }
    pthread_mutex_unlock(&g_read_mutex);
}

int read_cache_contains(const char *rel) {
    if (!rel || !rel[0]) return 0;
    int found = 0;
    pthread_mutex_lock(&g_read_mutex);
    for (int i = 0; i < g_read_count; i++) {
        if (strcmp(g_read_paths[i], rel) == 0) { found = 1; break; }
    }
    pthread_mutex_unlock(&g_read_mutex);
    return found;
}

void read_cache_clear(void) {
    pthread_mutex_lock(&g_read_mutex);
    g_read_count = 0;
    pthread_mutex_unlock(&g_read_mutex);
}

/* Return the full path to open for a read cache entry. If the entry
 * is absolute, it is opened as-is. Otherwise it is joined against
 * g_project.root. Writes the result into `out`.
 *
 * This is the read-side counterpart to the join logic used for
 * writes. Reads may reach outside the project root; writes may not. */
static void resolve_read_path(const char *rel, char *out, size_t out_sz) {
    if (!rel || !rel[0]) { out[0] = '\0'; return; }
    if (rel[0] == '/') {
        snprintf(out, out_sz, "%s", rel);
    } else {
        snprintf(out, out_sz, "%s/%s", g_project.root, rel);
    }
}

/* ================================================================== */
/*  Ignore rules                                                      */
/* ================================================================== */

static int is_ignored_dir(const char *name) {
    static const char *skip[] = {
        ".git", ".svn", ".hg", "node_modules", "build", "dist",
        "__pycache__", ".cache", ".venv", "venv", "target", ".wisp-code",
        NULL
    };
    for (int i = 0; skip[i]; i++) if (strcmp(name, skip[i]) == 0) return 1;
    return 0;
}

static int is_text_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    static const char *ok[] = {
        ".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh",
        ".py", ".js", ".ts", ".tsx", ".jsx", ".go", ".rs",
        ".java", ".kt", ".rb", ".sh", ".bash", ".zsh",
        ".txt", ".md", ".json", ".yaml", ".yml", ".toml", ".ini", ".cfg",
        ".cmake", ".mk", ".make", ".html", ".css", ".scss",
        ".sql", ".lua", ".php", ".pl", ".r", ".swift", ".m", ".mm", ".conf", ".run", ".macros",
        NULL
    };
    for (int i = 0; ok[i]; i++) if (strcasecmp(dot, ok[i]) == 0) return 1;
    return 0;
}

/* ================================================================== */
/*  Directory walk                                                    */
/* ================================================================== */
static int is_ignored_file(const char *name) {
    /* Build artifacts, editor leftovers, and logs are noise.
     * Config files are hidden from the model entirely: wisp-code.conf
     * holds the operator's api_key, model, endpoint, theme, and
     * timeout; wisp-code.run holds the model's lang and run keys.
     * The model reaches neither through the snapshot; it changes
     * lang or run by re-emitting the whole wisp-code.run file. */
    static const char *skip_ext[] = {
        ".rej", ".orig", ".log", ".conf",
        NULL
    };
    for (int i = 0; skip_ext[i]; i++) {
        size_t n = strlen(name);
        size_t e = strlen(skip_ext[i]);
        if (n > e && strcasecmp(name + n - e, skip_ext[i]) == 0) return 1;
    }
    return 0;
}

#if defined(_WIN32)

static int is_hidden_attr(const char *abs) {
    DWORD a = GetFileAttributesA(abs);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    return (a & FILE_ATTRIBUTE_HIDDEN) != 0;
}
#endif

static void walk_dir(const char *root, const char *rel, int *count) {
    char abs[MAX_PATH_LEN * 2 + 2];
    if (rel[0]) snprintf(abs, sizeof(abs), "%s/%s", root, rel);
    else        snprintf(abs, sizeof(abs), "%s", root);

    DIR *d = opendir(abs);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (*count >= MAX_FILES) break;

        /* Hidden entries are invisible to the model. A path component
         * starting with '.' is private: .env, .gitignore, .github/,
         * .vscode/, .config/, .ssh/, etc. The only exceptions are the
         * two directory-walk sentinels "." and ".." handled above. */
        if (e->d_name[0] == '.') continue;

        char child_rel[MAX_PATH_LEN];
        if (rel[0]) snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, e->d_name);
        else        snprintf(child_rel, sizeof(child_rel), "%s", e->d_name);

        char child_abs[MAX_PATH_LEN * 2 + 2];
        snprintf(child_abs, sizeof(child_abs), "%s/%s", root, child_rel);

        #if defined(_WIN32)
        if (is_hidden_attr(child_abs)) continue;
        #endif

        struct stat st;
        #if defined(_WIN32)
        if (stat(child_abs, &st) != 0) continue;
        #else
        if (lstat(child_abs, &st) != 0) continue;
        #endif        

        if (S_ISDIR(st.st_mode)) {
            if (is_ignored_dir(e->d_name)) continue;
            walk_dir(root, child_rel, count);
        } else if (S_ISREG(st.st_mode)) {
            if (is_ignored_file(e->d_name)) continue;
            if (!is_text_extension(e->d_name)) continue;
            file_entry *fe = &g_project.files[*count];
            snprintf(fe->path, sizeof(fe->path), "%s", child_rel);
            fe->size  = (size_t)st.st_size;
            fe->mtime = st.st_mtime;
            (*count)++;
        }
    }
    closedir(d);
}

static int cmp_file(const void *a, const void *b) {
    return strcmp(((const file_entry *)a)->path, ((const file_entry *)b)->path);
}

/* Return 1 if `path` is present in the baseline with the same
 * (size, mtime) as `cur`. Return 0 otherwise, including when the
 * baseline is invalid. */
static int file_matches_baseline(const file_entry *cur) {
    if (!g_project.last_sent_valid) return 0;
    for (int j = 0; j < g_project.last_sent.count; j++) {
        const file_entry *old = &g_project.last_sent.files[j];
        if (strcmp(old->path, cur->path) == 0) {
            return (old->size == cur->size && old->mtime == cur->mtime);
        }
    }
    return 0;
}

/* ================================================================== */
/*  Snapshot                                                          */
/* ================================================================== */

int build_snapshot(void) {
    free(g_project.snapshot);
    g_project.snapshot = NULL;
    g_project.snapshot_len = 0;
    g_project.file_count = 0;

    walk_dir(g_project.root, "", &g_project.file_count);
    qsort(g_project.files, g_project.file_count, sizeof(file_entry), cmp_file);

    size_t cap = 1 << 20;
    char *buf = malloc(cap);
    if (!buf) return -1;
    size_t used = 0;

    #define APPEND(fmt, ...) do {                                         \
        int _n = snprintf(buf + used, cap - used, fmt, ##__VA_ARGS__);    \
        if (_n < 0)                                                       \
        { free(buf); return -1; }                                         \
        if ((size_t)_n >= cap - used) {                                   \
            size_t newcap = cap * 2;                                      \
            while (newcap < used + (size_t)_n + 1) newcap *= 2;           \
            if (newcap > MAX_TREE_BYTES) { free(buf); return -1; }        \
            char *nb = realloc(buf, newcap);                              \
            if (!nb) { free(buf); return -1; }                            \
            buf = nb; cap = newcap;                                       \
            _n = snprintf(buf + used, cap - used, fmt, ##__VA_ARGS__);    \
        }                                                                 \
        used += (size_t)_n;                                               \
    } while (0)

    APPEND("# PROJECT SNAPSHOT\n");
    APPEND("# root: %s\n", g_project.root);
    APPEND("# files: %d\n\n", g_project.file_count);

    APPEND("## TREE\n");
    for (int i = 0; i < g_project.file_count; i++) {
        APPEND("%s\n", g_project.files[i].path);
    }
    APPEND("\n## CONTENTS\n\n");

    /* Delta-aware content loop.
     *
     * On the first turn (last_sent_valid == 0), every file is new and
     * its full contents are inlined. That is the "startup sends
     * everything" case.
     *
     * On later turns, files whose (size, mtime) match the baseline are
     * emitted as a one-line placeholder. Files that changed — user
     * edits, model writes since the last baseline capture, new files —
     * are inlined in full.
     *
     * The baseline lives in g_project.last_sent and is refreshed by
     * snapshot_capture_baseline() at the end of every successful turn,
     * including after the model's own writes. That is what makes
     * Route B work: a file the model just wrote is part of the
     * baseline, so it does not appear as a change next turn. */
    for (int i = 0; i < g_project.file_count; i++) {
        const file_entry *cur = &g_project.files[i];

        /* %%%READ overrides the baseline. If the model explicitly
         * asked for this file, inline it in full even if it is
         * unchanged. */
        if (!read_cache_contains(cur->path) && file_matches_baseline(cur)) {
            APPEND("### FILE %s (unchanged, %zu bytes)\n\n",
                   cur->path, cur->size);
            continue;
        }

        char abs[MAX_PATH_LEN * 2 + 2];
        snprintf(abs, sizeof(abs), "%s/%s", g_project.root, cur->path);

        FILE *f = fopen(abs, "rb");
        if (!f) {
            APPEND("### FILE %s\n(unreadable)\n\n", cur->path);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        if (sz < 0 || sz > MAX_FILE_BYTES) {
            fclose(f);
            APPEND("### FILE %s\n(too large)\n\n", cur->path);
            continue;
        }
        fseek(f, 0, SEEK_SET);
        char *body = malloc((size_t)sz + 1);
        if (!body) { fclose(f); free(buf); return -1; }
        size_t got = fread(body, 1, (size_t)sz, f);
        fclose(f);
        body[got] = '\0';

        APPEND("### FILE %s\n", cur->path);
        APPEND("%s\n", body);
        if (got && body[got - 1] != '\n') APPEND("\n");
        APPEND("\n");
        free(body);
    }

    #undef APPEND

    g_project.snapshot = buf;
    g_project.snapshot_len = used;
    return 0;
}

/* ================================================================== */
/*  Config file reader                                                */
/* ================================================================== */

static int conf_get_file(const char *root, const char *filename,
                         const char *key, char *out, size_t out_sz) {
    if (!root || !root[0] || !filename || !filename[0] ||
        !key || !key[0] || !out || out_sz == 0) {
        return -1;
    }
    out[0] = '\0';

    char path[MAX_PATH_LEN * 2 + 32];
    snprintf(path, sizeof(path), "%s/%s", root, filename);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    size_t klen = strlen(key);
    char line[4096];
    int found = -1;

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        if (strncmp(p, key, klen) != 0) continue;
        char *q = p + klen;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;

        char *ve = q + strlen(q);
        while (ve > q && (ve[-1] == '\n' || ve[-1] == '\r' ||
                          ve[-1] == ' '  || ve[-1] == '\t')) {
            *--ve = '\0';
        }

        size_t vlen = strlen(q);
        if (vlen >= out_sz) { found = -1; break; }
        memcpy(out, q, vlen);
        out[vlen] = '\0';
        found = 0;
        break;
    }

    fclose(f);
    return found;
}

int conf_get(const char *root, const char *key, char *out, size_t out_sz) {
    return conf_get_file(root, "wisp-code.conf", key, out, out_sz);
}

/* Read `key = value` from <root>/wisp-code.run. Same syntax as
 * conf_get(). The model owns wisp-code.run and rewrites it whole
 * whenever lang or run changes. */
int run_conf_get(const char *root, const char *key, char *out, size_t out_sz) {
    return conf_get_file(root, "wisp-code.run", key, out, out_sz);
}

void snapshot_capture_baseline(void) {
    g_project.last_sent.count = g_project.file_count;
    for (int i = 0; i < g_project.file_count; i++) {
        g_project.last_sent.files[i] = g_project.files[i];
    }
    g_project.last_sent_valid = 1;
}

/* ================================================================== */
/*  Change detection                                                  */
/*                                                                    */
/*  The baseline is a copy of g_project.files[] taken at the moment   */
/*  the last request went out. On the next request, we diff the live  */
/*  list against it and emit a delta block. Added and modified files  */
/*  carry their full current contents, so the model has ground truth  */
/*  without a %%%READ round-trip. Removed files are named only.       */
/*                                                                    */
/*  The model reconstructs its view of disk like this:                */
/*                                                                    */
/*      initial snapshot (sent once, turn 1)                          */
/*    + every delta since (appended to history, never re-sent)        */
/*    = current disk state                                            */
/*                                                                    */
/*  Because deltas accumulate in history and are never resent, the    */
/*  per-turn cost is O(change size), not O(project size).             */
/* ================================================================== */

void snapshot_emit_delta(char *buf, size_t buf_sz) {
    if (!buf || buf_sz == 0) return;
    buf[0] = '\0';
    if (!g_project.last_sent_valid) return;

    size_t used = 0;
    #define ADD(fmt, ...) do {                                           \
        int _n = snprintf(buf + used, buf_sz - used, fmt, ##__VA_ARGS__); \
        if (_n > 0) used += (size_t)_n;                                  \
        if (used >= buf_sz) { used = buf_sz - 1; buf[used] = '\0'; }     \
    } while (0)

    int any = 0;

    /* Both g_project.files[] and g_project.last_sent.files[] are sorted
     * by path — build_snapshot() qsorts the live list, and
     * snapshot_capture_baseline() copies it in order. So the added /
     * modified detection is a two-pointer merge, not a nested scan.
     * For 500 files this turns 250,000 strcmp calls per invocation
     * into 500. */

    int j = 0;   /* baseline cursor */

    /* Static, not stack: 32 KiB on the caller's stack is risky on
     * musl/Alpine (128 KiB default pthread stack). Safe from
     * concurrency because requests are serialized by g_busy — only
     * one thread ever reaches this function at a time.
     *
     * Must be cleared on every call: the array persists across
     * invocations, and stale marks would make the removed-files pass
     * skip baseline entries that were matched on a previous turn. */
    static int seen[MAX_FILES];
    memset(seen, 0, sizeof(int) * (size_t)g_project.last_sent.count);

    for (int i = 0; i < g_project.file_count; i++) {
        const file_entry *cur = &g_project.files[i];

        /* Advance the baseline cursor past entries whose path sorts
         * before the current path. Those baseline entries are "removed"
         * candidates — they may still match nothing later, so we do
         * not consume them here; the removed pass handles them. */
        while (j < g_project.last_sent.count &&
               strcmp(g_project.last_sent.files[j].path, cur->path) < 0) {
            j++;
        }

        int found = -1;
        if (j < g_project.last_sent.count &&
            strcmp(g_project.last_sent.files[j].path, cur->path) == 0) {
            found = j;
            seen[j] = 1;
            j++;   /* consume this baseline entry */
        }

        int changed = 0;
        int is_added = (found < 0);
        if (is_added) {
            changed = 1;
        } else {
            const file_entry *old = &g_project.last_sent.files[found];
            if (old->size != cur->size || old->mtime != cur->mtime) {
                changed = 1;
            }
        }

        /* A %%%READ queued by the model this turn also forces an
         * inline, even if the file matches the baseline. That path
         * still works and is the only way the model can see a file
         * it never wrote and that did not move. */
        int forced = read_cache_contains(cur->path);

        if (!changed && !forced) continue;

        if (!any) { ADD("[CHANGES]\n"); any = 1; }

        if (is_added) {
            ADD("added: %s\n", cur->path);
        } else if (forced && !changed) {
            ADD("read: %s\n", cur->path);
        } else {
            const file_entry *old = &g_project.last_sent.files[found];
            ADD("modified: %s (was %zu bytes, now %zu bytes)\n",
                cur->path, old->size, cur->size);
        }

        /* Inline the full current contents. The delta buffer is
         * bounded, so a large file (or a large combined delta) can
         * exceed it. When that happens, emit an explicit truncation
         * marker so the model knows it is looking at a partial view
         * and should %%%READ the file before patching it. Never emit
         * an unclosed <<<path block — the closing >>> must always
         * land, or the model will mis-parse the boundary between
         * this file and the next. */
        char abs[MAX_PATH_LEN * 2 + 2];
        snprintf(abs, sizeof(abs), "%s/%s", g_project.root, cur->path);

        FILE *f = fopen(abs, "rb");
        if (!f) {
            ADD("(unreadable)\n");
            continue;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        if (sz < 0 || sz > MAX_FILE_BYTES) {
            fclose(f);
            ADD("(too large)\n");
            continue;
        }
        fseek(f, 0, SEEK_SET);

        /* Reserve room for:
         *   - the opening marker "  <<<path\n"
         *   - the closing marker "  >>>\n"
         *   - a truncation notice "  ... [truncated]\n"
         * If less than that is available, emit a one-line placeholder
         * instead of an unterminated block. */
        {
            size_t room = buf_sz - used;
            size_t need = strlen(cur->path) + 16   /* <<<path\n */
                        + 16                        /* >>>\n */
                        + 64;                       /* truncation notice */
            if (room < need + 256) {
                fclose(f);
                ADD("(omitted: delta buffer full — file too large to inline)\n");
                continue;
            }
        }

        ADD("<<<%s\n", cur->path);

        /* Leave headroom so the closing "\n>>>\n" always fits even
         * after the truncation notice. */
        size_t body_limit = buf_sz - used - 80;
        char chunk[4096];
        size_t got;
        int truncated = 0;

        while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
            if (used + got > body_limit) {
                size_t fit = body_limit - used;
                if (fit > 0) {
                    memcpy(buf + used, chunk, fit);
                    used += fit;
                    buf[used] = '\0';
                }
                truncated = 1;
                break;
            }
            memcpy(buf + used, chunk, got);
            used += got;
            buf[used] = '\0';
        }
        fclose(f);

        if (used > 0 && buf[used - 1] != '\n') ADD("\n");
        if (truncated) {
            ADD("... [truncated — %ld bytes total, read the file to see the rest]\n",
                sz);
        }
        ADD(">>>\n");
    }

    /* Removed files: baseline entries we never matched, in sorted
     * order. Walk the seen[] table — no per-entry live scan. */
    for (int k = 0; k < g_project.last_sent.count; k++) {
        if (seen[k]) continue;
        const file_entry *old = &g_project.last_sent.files[k];
        if (!any) { ADD("[CHANGES]\n"); any = 1; }
        ADD("removed: %s\n", old->path);
    }

    if (any) {
        ADD("[/CHANGES]\n\n");
    }
    #undef ADD
}

/* ================================================================== */
/*  Shell-mode reads                                                  */
/*                                                                    */
/*  Emits the files the model asked for this turn via %%%READ,        */
/*  inlined in full, with no [CHANGES] framing and no tree. Absolute  */
/*  read paths open directly; relative paths are joined against       */
/*  g_project.root.                                                   */
/*                                                                    */
/*  This is the shell-mode counterpart to snapshot_emit_delta(). The  */
/*  read cache is populated by run_reads_from_reply() in reply.c, the */
/*  same way it is in code mode. The cache is cleared by              */
/*  request_thread after a request is composed.                       */
/* ================================================================== */

void snapshot_emit_reads(char *buf, size_t buf_sz) {
    if (!buf || buf_sz == 0) return;
    buf[0] = '\0';

    size_t used = 0;
    #define ADD(fmt, ...) do {                                           \
        int _n = snprintf(buf + used, buf_sz - used, fmt, ##__VA_ARGS__); \
        if (_n > 0) used += (size_t)_n;                                  \
        if (used >= buf_sz) { used = buf_sz - 1; buf[used] = '\0'; }     \
    } while (0)

    /* Snapshot the cache under the mutex, then release before any I/O. */
    char paths[READ_CACHE_MAX][MAX_PATH_LEN];
    int n = 0;
    pthread_mutex_lock(&g_read_mutex);
    n = g_read_count;
    if (n > READ_CACHE_MAX) n = READ_CACHE_MAX;
    for (int i = 0; i < n; i++) {
        snprintf(paths[i], MAX_PATH_LEN, "%s", g_read_paths[i]);
    }
    pthread_mutex_unlock(&g_read_mutex);

    if (n == 0) return;

    ADD("--- READS ---\n");

    for (int i = 0; i < n; i++) {
        char abs[MAX_PATH_LEN * 2 + 2];
        resolve_read_path(paths[i], abs, sizeof(abs));

        FILE *f = fopen(abs, "rb");
        if (!f) {
            ADD("<<<%s\n", paths[i]);
            ADD("(unreadable: file not found or permission denied)\n");
            ADD(">>>\n");
            continue;
        }

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        if (sz < 0 || sz > MAX_FILE_BYTES) {
            fclose(f);
            ADD("<<<%s\n", paths[i]);
            ADD("(too large: exceeds %d bytes)\n", MAX_FILE_BYTES);
            ADD(">>>\n");
            continue;
        }
        fseek(f, 0, SEEK_SET);

        /* Reserve room for the opening marker, the closing marker,
         * and a truncation notice. If there isn't enough headroom,
         * emit a one-line placeholder instead of an unterminated
         * block. */
        {
            size_t room = buf_sz - used;
            size_t need = strlen(paths[i]) + 16 + 16 + 96;
            if (room < need + 256) {
                fclose(f);
                ADD("<<<%s\n", paths[i]);
                ADD("(omitted: read buffer full — file too large to inline)\n");
                ADD(">>>\n");
                continue;
            }
        }

        ADD("<<<%s\n", paths[i]);

        /* Leave headroom so "\n>>>\n" always fits even after a
         * truncation notice. */
        size_t body_limit = buf_sz - used - 80;
        char chunk[4096];
        size_t got;
        int truncated = 0;

        while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
            if (used + got > body_limit) {
                size_t fit = body_limit - used;
                if (fit > 0) {
                    memcpy(buf + used, chunk, fit);
                    used += fit;
                    buf[used] = '\0';
                }
                truncated = 1;
                break;
            }
            memcpy(buf + used, chunk, got);
            used += got;
            buf[used] = '\0';
        }
        fclose(f);

        if (used > 0 && buf[used - 1] != '\n') ADD("\n");
        if (truncated) {
            ADD("... [truncated — %ld bytes total]\n", sz);
        }
        ADD(">>>\n");
    }

    ADD("--- END READS ---\n\n");
    #undef ADD
}

/* ================================================================== */
/*  Write                                                             */
/* ================================================================== */

int path_is_safe(const char *rel) {
    if (!rel || !rel[0]) return -1;
    if (rel[0] == '/') return -1;

    const char *s = rel;
    while (*s) {
        const char *slash = strchr(s, '/');
        size_t seg_len = slash ? (size_t)(slash - s) : strlen(s);
        if (seg_len == 2 && s[0] == '.' && s[1] == '.') return -1;
        if (seg_len == 1 && s[0] == '.') return -1;
        if (seg_len == 0) return -1;
        s = slash ? slash + 1 : s + seg_len;
    }
    return 0;
}

/* Returns 1 if any path component starts with '.', excluding the
 * special names "." and "..". Both are legitimate path syntax
 * (current directory, parent directory) and appear in normalized
 * paths, so they're allowed. Everything else starting with '.' is
 * considered hidden and rejected. */
static int path_has_hidden_component(const char *p)
{
    if (!p) return 0;
    const char *seg = p;
    for (const char *q = p; ; q++) {
        if (*q == '/' || *q == 0) {
            size_t len = (size_t)(q - seg);
            if (len > 0 && seg[0] == '.') {
                int is_dot  = (len == 1);
                int is_dotdot = (len == 2 && seg[1] == '.');
                if (!is_dot && !is_dotdot) return 1;
            }
            if (*q == 0) break;
            seg = q + 1;
        }
    }
    return 0;
}

/* Read-side check. Absolute paths are allowed — a shell agent needs
 * to read /etc/php/... directly. Relative paths fall through to the
 * write-side check, which is stricter about `..` and empty segments.
 *
 * This is used by read_cache_add() and run_reads_from_reply() in
 * reply.c. It is NOT used for writes. */
int path_is_safe_read(const char *p) {
    if (!p || !p[0]) return -1;

    if (p[0] == '/') {
        /* Absolute reads allowed. Still reject anything with a
         * backtick, newline, or hidden component. */
        for (const char *q = p; *q; q++) {
            if (*q == '`' || *q == '\n' || *q == '\r') return -1;
        }
        if (path_has_hidden_component(p)) return -1;
        return 0;
    }

    if (path_is_safe(p) != 0) return -1;

    for (const char *q = p; *q; q++) {
        if (*q == '`' || *q == '\n' || *q == '\r') return -1;
    }

    if (path_has_hidden_component(p)) return -1;

    /* A "path" with no alphanumeric character is parser garbage. */
    {
        int has_alpha = 0;
        for (const char *q = p; *q; q++) {
            if ((*q >= 'a' && *q <= 'z') ||
                (*q >= 'A' && *q <= 'Z') ||
                (*q >= '0' && *q <= '9') ||
                *q == '_' || *q == '.' || *q == '-' || *q == '/') {
                has_alpha = 1;
                break;
            }
        }
        if (!has_alpha) return -1;
    }
    return 0;
}


int write_one_file(const char *rel, const char *body, size_t body_len) {
    if (path_is_safe(rel) != 0) {
        ops_pushf("skip %s (unsafe path)", rel ? rel : "(null)");
        return -1;
    }

    char abs[MAX_PATH_LEN * 2 + 2];
    snprintf(abs, sizeof(abs), "%s/%s", g_project.root, rel);

    char tmp[MAX_PATH_LEN * 2 + 2];
    snprintf(tmp, sizeof(tmp), "%s", abs);
    for (char *q = tmp + 1; *q; q++) {
        if (*q == '/') {
            *q = '\0';
            mkdir(tmp, 0755);
            *q = '/';
        }
    }

    FILE *f = fopen(abs, "wb");
    if (!f) {
        ops_pushf("write failed: %s: %s", rel, strerror(errno));
        return -1;
    }
    if (body_len) fwrite(body, 1, body_len, f);
    fclose(f);
    /* Caller (run_files_from_markers in reply.c) pushes the display
     * line so multiple writes can be grouped into one foldable
     * section: [DONE] applied N files, body = one "wrote X (N bytes)"
     * line per file. */
    return 0;
}
