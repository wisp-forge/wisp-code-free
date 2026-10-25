// language: C, file: wisp-code/reply.c
#include "console.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ================================================================== */
/*  Block markers                                                     */
/* ================================================================== */

#define M_FILE_OPEN  "%%%FILE"
#define M_MSG_OPEN   "%%%MSG"
#define M_JUMP_OPEN  "%%%JUMP"
#define M_READ_OPEN  "%%%READ"
#define M_TERM       "%%%END"

/* Return values from process_reply. */
#define REPLY_OK          0
#define REPLY_READ_NEXT   1

/* Defined at the bottom of this file, called from process_reply()
 * above. Forward-declared here so the compiler sees the prototype
 * before the call site. */
int reply_wants_continue(const char *content);

/* Every opener, terminated with NULL. Used by find_block_end() and
 * by the MSG parser's next-block scan. */
static const char *BLOCK_OPENERS[] = {
    M_FILE_OPEN, M_MSG_OPEN, M_JUMP_OPEN, M_READ_OPEN, M_TERM, NULL
};

/* ================================================================== */
/*  Fence stripping                                                   */
/* ================================================================== */

static const char *strip_fence(const char *body, size_t *out_len) {
    if (!body || *out_len == 0) return body;

    const char *start = body;
    const char *end = body + *out_len;

    int leading = 0;
    if (*out_len >= 3 &&
        ((start[0] == '`' && start[1] == '`' && start[2] == '`') ||
         (start[0] == '~' && start[1] == '~' && start[2] == '~'))) {
        const char *nl = memchr(start, '\n', (size_t)(end - start));
        if (nl) {
            start = nl + 1;
            leading = 1;
        }
    }

    if (leading) {
        const char *p = end;
        while (p > start && (p[-1] == '\n' || p[-1] == '\r' ||
                             p[-1] == ' '  || p[-1] == '\t')) {
            p--;
        }
        if (p - start >= 3 &&
            ((p[-1] == '`' && p[-2] == '`' && p[-3] == '`') ||
             (p[-1] == '~' && p[-2] == '~' && p[-3] == '~'))) {
            const char *q = p - 3;
            if (q == start || q[-1] == '\n') {
                end = q;
                while (end > start &&
                       (end[-1] == '\n' || end[-1] == '\r' ||
                        end[-1] == ' '  || end[-1] == '\t')) {
                    end--;
                }
            }
        }
    }

    *out_len = (size_t)(end - start);
    return start;
}

/* ================================================================== */
/*  Path sanitization                                                 */
/* ================================================================== */

static int sanitize_path(const char *in, size_t in_len,
                         char *out, size_t out_sz) {
    if (!in || in_len == 0 || out_sz == 0) return -1;

    size_t i = 0;
    while (i < in_len && (in[i] == ' '  || in[i] == '\t' ||
                          in[i] == '"'  || in[i] == '\'' ||
                          in[i] == '`'  || in[i] == '<'  ||
                          in[i] == '>'  || in[i] == '['  ||
                          in[i] == '(')) {
        i++;
    }

    size_t j = i;
    while (j < in_len) {
        char c = in[j];
        if (c == ' '  || c == '\t' || c == '\r' || c == '\n' ||
            c == '"'  || c == '\'' || c == '`'  ||
            c == '<'  || c == '>'  || c == '['  ||
            c == ']'  || c == '('  || c == ')'  ||
            c == '{'  || c == '}'  || c == '#'  ||
            c == ';'  || c == ','  || c == '*'  ||
            c == '?'  || c == '|'  || c == ':') {
            break;
        }
        j++;
    }

    size_t len = j - i;
    if (len == 0 || len >= out_sz) return -1;

    memcpy(out, in + i, len);
    out[len] = '\0';

    if (strcmp(out, ".") == 0 || strcmp(out, "..") == 0) return -1;
    return 0;
}

/* ================================================================== */
/*  Block boundary detection                                          */
/* ================================================================== */

static int is_block_start(const char *scan, const char *body) {
    if (scan != body && scan[-1] != '\n') return 0;

    int in_fence = 0;
    const char *p = body;
    while (p < scan) {
        const char *eol = memchr(p, '\n', (size_t)(scan - p));
        const char *line_end = eol ? eol : scan;
        const char *q = p;
        while (q < line_end && (*q == ' ' || *q == '\t')) q++;
        if (line_end - q >= 3 &&
            ((q[0] == '`' && q[1] == '`' && q[2] == '`') ||
             (q[0] == '~' && q[1] == '~' && q[2] == '~'))) {
            in_fence = !in_fence;
        }
        p = eol ? eol + 1 : scan;
    }
    if (in_fence) return 0;

    p = scan;
    while (*p && *p != '\n' && *p != ' ' && *p != '\t') p++;
    if (*p == '\n' || *p == '\0') return 1;

    if (*p != ' ' && *p != '\t') return 0;

    while (*p == ' ' || *p == '\t') p++;
    int words = 0;
    while (*p && *p != '\n') {
        if (*p == ' ' || *p == '\t') {
            words++;
            while (*p == ' ' || *p == '\t') p++;
            continue;
        }
        p++;
    }
    return words <= 1;
}

static const char *find_block_end(const char *from) {
    const char *best = NULL;
    for (int i = 0; BLOCK_OPENERS[i]; i++) {
        const char *p = from;
        while ((p = strstr(p, BLOCK_OPENERS[i])) != NULL) {
            if (is_block_start(p, from)) {
                if (!best || p < best) best = p;
                break;
            }
            p += strlen(BLOCK_OPENERS[i]);
        }
    }
    return best;
}

/* ================================================================== */
/*  FILE block parser                                                 */
/* ================================================================== */

static void load_first(const char *path, int *flag) {
    if (!*flag) {
        code_load_file(path);
        *flag = 1;
    }
}

int run_files_from_markers(const char *reply, int *written_out) {
    if (written_out) *written_out = 0;
    if (!reply) return -1;

    int written = 0;
    int first_loaded = 0;
    const char *p = reply;
    const size_t OPEN_LEN = strlen(M_FILE_OPEN);

    char   bodybuf[16384];
    size_t body_used = 0;
    bodybuf[0] = '\0';

    while (*p) {
        const char *m = strstr(p, M_FILE_OPEN);
        if (!m) break;
        if (!is_block_start(m, reply)) { p = m + OPEN_LEN; continue; }

        const char *nl = strchr(m, '\n');
        if (!nl) break;

        const char *raw_begin = m + OPEN_LEN;
        size_t      raw_len   = (size_t)(nl - raw_begin);

        char path[MAX_PATH_LEN];
        if (sanitize_path(raw_begin, raw_len, path, sizeof(path)) != 0) {
            wisp_logf("reply | skipping FILE block with unsanitizable path");
            ops_push_section(SEC_FAIL,
                             "skipped a FILE block with an invalid path",
                             "", NULL);
            const char *skip_end = find_block_end(nl + 1);
            if (!skip_end) break;
            p = skip_end;
            continue;
        }

        const char *body = nl + 1;

        const char *body_end = find_block_end(body);
        int implicit_end = 0;
        if (!body_end) {
            body_end = body + strlen(body);
            implicit_end = 1;
        } else if (strncmp(body_end, M_TERM, strlen(M_TERM)) != 0) {
            implicit_end = 1;
        }

        size_t body_len = (size_t)(body_end - body);
        while (body_len > 0 &&
               (body[body_len - 1] == '\n' ||
                body[body_len - 1] == '\r' ||
                body[body_len - 1] == ' ' ||
                body[body_len - 1] == '\t')) {
            body_len--;
        }

        while (body_len > 0 &&
               (*body == ' '  || *body == '\t' ||
                *body == '\r' || *body == '\n')) {
            body++;
            body_len--;
        }

        body = strip_fence(body, &body_len);

        while (body_len > 0 &&
               (body[body_len - 1] == '\n' ||
                body[body_len - 1] == '\r' ||
                body[body_len - 1] == ' ' ||
                body[body_len - 1] == '\t')) {
            body_len--;
        }

        char *with_nl = malloc(body_len + 2);
        if (with_nl) {
            memcpy(with_nl, body, body_len);
            if (body_len == 0 || with_nl[body_len - 1] != '\n') {
                with_nl[body_len++] = '\n';
            }
            with_nl[body_len] = '\0';

            wisp_logf("reply | FILE block: path='%s' body=%zu bytes%s",
                       path, body_len,
                       implicit_end ? " (unterminated)" : "");

            if (write_one_file(path, with_nl, body_len) == 0) {
                written++;
                load_first(path, &first_loaded);
                if (body_used < sizeof(bodybuf) - 128) {
                    int n = snprintf(bodybuf + body_used,
                                     sizeof(bodybuf) - body_used,
                                     "wrote %s (%zu bytes)\n",
                                     path, body_len);
                    if (n > 0) body_used += (size_t)n;
                }
            }
            free(with_nl);
        }

        if (implicit_end) {
            if (!*body_end) break;
            p = body_end;
        } else {
            p = body_end + strlen(M_TERM);
        }
    }

    if (written > 0) {
        char label[128];
        snprintf(label, sizeof(label), "applied %d file%s",
                 written, written == 1 ? "" : "s");

        tr_push_section(SEC_DONE, label, "", bodybuf);

        if (written_out) *written_out = written;
        build_snapshot();
        return 0;
    }
    return -1;
}

/* ================================================================== */
/*  READ block parser                                                 */
/* ================================================================== */

static int run_reads_from_reply(const char *reply) {
    if (!reply) return 0;

    int pushed = 0;
    const char *p = reply;
    const size_t OPEN_LEN = strlen(M_READ_OPEN);

    while (*p) {
        const char *m = strstr(p, M_READ_OPEN);
        if (!m) break;
        if (!is_block_start(m, reply)) { p = m + OPEN_LEN; continue; }

        const char *nl = strchr(m, '\n');
        if (!nl) break;
        const char *body = nl + 1;

        const char *body_end = find_block_end(body);
        int implicit_end = 0;
        if (!body_end) {
            body_end = body + strlen(body);
            implicit_end = 1;
        } else if (strncmp(body_end, M_TERM, strlen(M_TERM)) != 0) {
            implicit_end = 1;
        }

        int handled = 0;
        {
            const char *hdr = m + OPEN_LEN;
            size_t hlen = (size_t)(nl - hdr);
            while (hlen > 0 && (*hdr == ' ' || *hdr == '\t')) {
                hdr++;
                hlen--;
            }
            while (hlen > 0 &&
                   (hdr[hlen - 1] == ' ' || hdr[hlen - 1] == '\t' ||
                    hdr[hlen - 1] == '\r')) {
                hlen--;
            }

            if (hlen > 0 && hlen < MAX_PATH_LEN) {
                char hpath[MAX_PATH_LEN];
                memcpy(hpath, hdr, hlen);
                hpath[hlen] = '\0';

                if (hpath[0] != '/' &&
                    (hpath[0] == 'a' || hpath[0] == 'b') && hpath[1] == '/') {
                    memmove(hpath, hpath + 2, strlen(hpath + 2) + 1);
                }

                if (path_is_safe_read(hpath) == 0) {
                    wisp_logf("reply | READ block (header arg): queueing '%s'",
                               hpath);
                    read_cache_add(hpath);
                    pushed++;
                    handled = 1;
                } else {
                    wisp_logf("reply | READ header arg '%s' rejected", hpath);
                    ops_push_section(SEC_FAIL,
                                     "skipped READ with malformed path",
                                     "", NULL);
                    handled = 1;
                }
            }
        }

        if (!handled) {
            size_t len = (size_t)(body_end - body);

            while (len > 0 &&
                   (body[len - 1] == '\n' ||
                    body[len - 1] == '\r' ||
                    body[len - 1] == ' ' ||
                    body[len - 1] == '\t')) {
                len--;
            }

            while (len > 0 &&
                   (*body == ' ' || *body == '\t' ||
                    *body == '\r' || *body == '\n')) {
                body++;
                len--;
            }

            body = strip_fence(body, &len);

            while (len > 0 &&
                   (body[len - 1] == '\n' ||
                    body[len - 1] == '\r' ||
                    body[len - 1] == ' ' ||
                    body[len - 1] == '\t')) {
                len--;
            }

            const char *spec = body;
            size_t spec_len = len;
            while (spec_len > 0 &&
                   (*spec == ' ' || *spec == '\t' ||
                    *spec == '\r' || *spec == '\n')) {
                spec++;
                spec_len--;
            }
            const char *spec_eol = memchr(spec, '\n', spec_len);
            if (spec_eol) spec_len = (size_t)(spec_eol - spec);
            while (spec_len > 0 &&
                   (spec[spec_len - 1] == ' ' ||
                    spec[spec_len - 1] == '\t' ||
                    spec[spec_len - 1] == '\r')) {
                spec_len--;
            }

            if (spec_len == 0 || spec_len >= MAX_PATH_LEN) {
                wisp_logf("reply | READ block: empty or oversized path");
            } else {
                char path[MAX_PATH_LEN];
                memcpy(path, spec, spec_len);
                path[spec_len] = '\0';

                if (path[0] != '/' &&
                    (path[0] == 'a' || path[0] == 'b') && path[1] == '/') {
                    memmove(path, path + 2, strlen(path + 2) + 1);
                }

                if (path_is_safe_read(path) != 0) {
                    wisp_logf("reply | READ block: rejected unsafe path '%s'",
                               path);
                    ops_push_section(SEC_FAIL,
                                     "skipped a READ block for unsafe path",
                                     "", NULL);
                } else {
                    wisp_logf("reply | READ block: queueing '%s'", path);
                    read_cache_add(path);
                    pushed++;
                }
            }
        }

        if (implicit_end) {
            if (!*body_end) break;
            p = body_end;
        } else {
            p = body_end + strlen(M_TERM);
        }
    }

    return pushed;
}

/* ================================================================== */
/*  JUMP block parser                                                 */
/* ================================================================== */

static int run_jumps_from_reply(const char *reply) {
    if (!reply) return 0;

    int pushed = 0;
    const char *p = reply;
    const size_t OPEN_LEN = strlen(M_JUMP_OPEN);

    while (*p) {
        const char *m = strstr(p, M_JUMP_OPEN);
        if (!m) break;
        if (!is_block_start(m, reply)) { p = m + OPEN_LEN; continue; }

        const char *nl = strchr(m, '\n');
        if (!nl) break;
        const char *body = nl + 1;

        const char *body_end = find_block_end(body);
        int implicit_end = 0;
        if (!body_end) {
            body_end = body + strlen(body);
            implicit_end = 1;
        } else if (strncmp(body_end, M_TERM, strlen(M_TERM)) != 0) {
            implicit_end = 1;
        }

        char spec[MAX_PATH_LEN + 16];
        size_t spec_len = 0;
        int have_spec = 0;

        {
            const char *hdr = m + OPEN_LEN;
            size_t hlen = (size_t)(nl - hdr);
            while (hlen > 0 && (*hdr == ' ' || *hdr == '\t')) {
                hdr++;
                hlen--;
            }
            while (hlen > 0 &&
                   (hdr[hlen - 1] == ' ' || hdr[hlen - 1] == '\t' ||
                    hdr[hlen - 1] == '\r')) {
                hlen--;
            }
            if (hlen > 0 && hlen < sizeof(spec)) {
                memcpy(spec, hdr, hlen);
                spec[hlen] = '\0';
                spec_len = hlen;
                have_spec = 1;
            }
        }

        if (!have_spec) {
            size_t len = (size_t)(body_end - body);
            while (len > 0 &&
                   (body[len - 1] == '\n' ||
                    body[len - 1] == '\r' ||
                    body[len - 1] == ' ' ||
                    body[len - 1] == '\t')) {
                len--;
            }
            while (len > 0 &&
                   (*body == ' ' || *body == '\t' ||
                    *body == '\r' || *body == '\n')) {
                body++;
                len--;
            }
            body = strip_fence(body, &len);
            while (len > 0 &&
                   (body[len - 1] == '\n' ||
                    body[len - 1] == '\r' ||
                    body[len - 1] == ' ' ||
                    body[len - 1] == '\t')) {
                len--;
            }

            const char *s = body;
            size_t slen = len;
            while (slen > 0 &&
                   (*s == ' ' || *s == '\t' ||
                    *s == '\r' || *s == '\n')) {
                s++;
                slen--;
            }
            const char *eol = memchr(s, '\n', slen);
            if (eol) slen = (size_t)(eol - s);
            while (slen > 0 &&
                   (s[slen - 1] == ' ' || s[slen - 1] == '\t' ||
                    s[slen - 1] == '\r')) {
                slen--;
            }

            if (slen > 0 && slen < sizeof(spec)) {
                memcpy(spec, s, slen);
                spec[slen] = '\0';
                spec_len = slen;
                have_spec = 1;
            }
        }

        if (!have_spec || spec_len == 0) {
            wisp_logf("reply | JUMP block with empty body");
            if (implicit_end) {
                if (!*body_end) break;
                p = body_end;
            } else {
                p = body_end + strlen(M_TERM);
            }
            continue;
        }

        const char *colon = NULL;
        for (const char *q = spec + spec_len - 1; q > spec; q--) {
            if (*q == ':') { colon = q; break; }
        }

        int ok = 1;
        if (!colon || colon == spec || colon + 1 >= spec + spec_len) {
            wisp_logf("reply | JUMP block missing ':' or line");
            ok = 0;
        }

        char path[MAX_PATH_LEN];
        int line = 0;
        if (ok) {
            size_t path_len = (size_t)(colon - spec);
            const char *line_str = colon + 1;
            size_t line_len = (size_t)((spec + spec_len) - line_str);

            int all_digits = 1;
            for (size_t i = 0; i < line_len; i++) {
                if (line_str[i] < '0' || line_str[i] > '9') {
                    all_digits = 0;
                    break;
                }
            }
            if (!all_digits || line_len == 0) {
                wisp_logf("reply | JUMP block line is not numeric");
                ok = 0;
            } else if (path_len == 0 || path_len >= sizeof(path)) {
                wisp_logf("reply | JUMP block path too long");
                ok = 0;
            } else {
                memcpy(path, spec, path_len);
                path[path_len] = '\0';
                for (size_t i = 0; i < line_len; i++) {
                    line = line * 10 + (line_str[i] - '0');
                    if (line > 1000000) line = 1000000;
                }
            }
        }

        if (ok) {
            code_jump_to(path, line);
            pushed++;
        }

        if (implicit_end) {
            if (!*body_end) break;
            p = body_end;
        } else {
            p = body_end + strlen(M_TERM);
        }
    }

    return pushed;
}

/* ================================================================== */
/*  MSG block parser                                                  */
/* ================================================================== */

static int run_messages_from_reply(const char *reply) {
    if (!reply) return 0;

    int pushed = 0;
    const char *p = reply;
    const size_t OPEN_LEN = strlen(M_MSG_OPEN);

    while (*p) {
        const char *m = strstr(p, M_MSG_OPEN);
        if (!m) break;
        if (!is_block_start(m, reply)) { p = m + OPEN_LEN; continue; }

        const char *nl = strchr(m, '\n');
        if (!nl) break;
        const char *body = nl + 1;

        const char *end = find_block_end(body);

        int stopped_on_terminator = 0;
        size_t len;
        if (!end) {
            len = strlen(body);
        } else {
            len = (size_t)(end - body);
            if (strncmp(end, M_TERM, strlen(M_TERM)) == 0) {
                stopped_on_terminator = 1;
            }
        }

        while (len > 0 &&
               (body[len - 1] == '\n' ||
                body[len - 1] == '\r' ||
                body[len - 1] == ' ' ||
                body[len - 1] == '\t')) {
            len--;
        }

        const char *body_after_fence = strip_fence(body, &len);

        if (len > 0) {
            char *copy = malloc(len + 1);
            if (copy) {
                memcpy(copy, body_after_fence, len);
                copy[len] = '\0';

                char meta[64];
                meta[0] = '\0';
                if (g_last_reply_bytes > 0) {
                    snprintf(meta, sizeof(meta), "%d bytes",
                             g_last_reply_bytes);
                }

                tr_push_section(SEC_SAYS, "", meta, copy);
                free(copy);
                pushed++;
            }
        }

        if (!end) break;
        if (stopped_on_terminator) {
            p = end + strlen(M_TERM);
        } else {
            p = end;
        }
    }

    return pushed;
}

/* ================================================================== */
/*  Reply dispatcher                                                  */
/*                                                                    */
/*  Returns:                                                          */
/*    REPLY_OK         — nothing further needed                       */
/*    REPLY_READ_NEXT  — reply was exclusively %%%READ; continue with */
/*                       the requested files inlined                  */
/* ================================================================== */

int process_reply(const char *content_in) {

    if (!content_in) return REPLY_OK;

    char *content = strdup(content_in);
    if (!content) return REPLY_OK;

    const char *p = content;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p == '{' && strstr(content, "\"choices\"")) {
        wisp_logf("reply | WARNING: process_reply got a raw JSON envelope");
        tr_push_error("[internal: reply parser got raw JSON, not content]");
        tr_push_error("[this is a bug — the reply had no message.content field]");
        g_last_error = 1;
        free(content);
        return REPLY_OK;
    }

    int wrote_files = 0;
    run_files_from_markers(content, &wrote_files);

    int messages_pushed = run_messages_from_reply(content);
    int jumps_taken     = run_jumps_from_reply(content);
    int reads_queued    = run_reads_from_reply(content);

    if (wrote_files > 0) {
        /* The "applied N files" section is pushed inside
         * run_files_from_markers. Nothing more to do here. */
    } else if (reads_queued > 0) {
        char label[128];
        snprintf(label, sizeof(label), "queued %d file%s for read",
                 reads_queued, reads_queued == 1 ? "" : "s");
        ops_push_section(SEC_PENDING, label, "", NULL);
    } else if (messages_pushed > 0) {
        /* nothing extra */
    } else if (jumps_taken > 0) {
        /* a jump with no accompanying text */
    } else {
        if (strlen(content) > 200) {
            char snippet[201];
            memcpy(snippet, content, 200);
            snippet[200] = '\0';
            tr_push_errorf("[reply had no block the parser recognized — "
                           "first 200 bytes shown, full body in log]");
            tr_push_block(snippet);
        } else {
            tr_push_block(content);
        }
        g_last_error = 0;
    }

    int want_read_continue = reply_wants_continue(content);

    tr_focus_tail();

    free(content);

    if (want_read_continue) {
        return REPLY_READ_NEXT;
    }
    return REPLY_OK;
}

/* ================================================================== */
/*  Auto-continue detection                                           */
/* ================================================================== */

int reply_wants_continue(const char *content) {
    if (!content) return 0;
    if (!strstr(content, M_READ_OPEN)) return 0;
    if (strstr(content, M_FILE_OPEN))  return 0;
    if (strstr(content, M_JUMP_OPEN))  return 0;
    /* %%%MSG does not cancel a READ. The model routinely narrates
     * "let me look at that file first" and emits the %%%READ in the
     * same reply. Cancelling on %%%MSG drops the read on the floor
     * and the model sees no file bytes on the follow-up turn. */
    return 1;
}