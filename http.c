// language: C, file: wisp-code/http.c
#include "console.h"
#include "prompt.h"
#include "platform.h"
#include "settings.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif


/* ================================================================== */
/*  Endpoint classification                                           */
/* ================================================================== */

static int endpoint_is_relay(void) {
    const char *url = settings_api_url();
    if (!url || !url[0]) return 0;

    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;
    else return 0;

    int is_loopback = 0;
    if (strncmp(p, "localhost:", 10) == 0) {
        is_loopback = 1;
    } else if (strncmp(p, "127.0.0.1:", 10) == 0) {
        is_loopback = 1;
    } else if (strncmp(p, "[::1]:", 6) == 0) {
        is_loopback = 1;
    } else {
        return 0;
    }

    const char *colon = strchr(p, ':');
    if (!colon) return 0;
    const char *port_s = colon + 1;
    if (port_s[0] < '0' || port_s[0] > '9') return 0;

    int port = 0;
    for (const char *q = port_s;
         *q >= '0' && *q <= '9' && port < 100000;
         q++) {
        port = port * 10 + (*q - '0');
    }

    return is_loopback && port >= 35673 && port <= 35688;
}

/* ================================================================== */
/*  HTTP POST — transport-agnostic                                    */
/*                                                                    */
/*  Lifetime rule: mg_connection is owned by mongoose. The moment    */
/*  mg_mgr_poll returns, the connection may have been freed or        */
/*  recycled. The poll loop below NEVER touches `c`. All state that   */
/*  needs to survive the connection lives in http_ctx_t.              */
/* ================================================================== */

static int dechunk(const char *body, size_t body_len,
                   char **out, size_t *out_len);

typedef struct {
    http_reply_t *r;
    int           closed;
    int           finished;      /* terminal event fired; stop polling */
    int           http_msg_seen;
    int           error_seen;
    int           cancel_requested;   /* main loop -> callback: close now */
    const char   *url;
} http_ctx_t;

static void finalize_body(http_ctx_t *ctx, const char *buf, size_t len) {
    http_reply_t *r = ctx->r;
    free(r->body);
    r->body = malloc(len + 1);
    if (!r->body) { r->len = 0; return; }
    memcpy(r->body, buf, len);
    r->body[len] = '\0';
    r->len = len;
}

static void http_cb(struct mg_connection *c, int ev, void *ev_data) {
    http_ctx_t *ctx = (http_ctx_t *)c->fn_data;
    http_reply_t *r = ctx->r;

    /* The main loop asked us to abort. `c` is valid here — this is
     * the only place it's safe to touch it. Mark for close and let
     * mongoose drive the teardown through MG_EV_CLOSE. */
    /* The main loop asked us to abort. `c` is valid here — this is
     * the only place it's safe to touch it. */
    if (ctx->cancel_requested) {
        /* Force RST instead of FIN so the relay's kernel sees the
         * disconnect immediately. c->fd is opaque in this mongoose
         * version; convert it to a socket handle via intptr_t. */
        int sock = (int)(intptr_t)c->fd;
        if (sock >= 0) {
            struct linger ling;
            ling.l_onoff  = 1;
            ling.l_linger = 0;
            setsockopt(sock, SOL_SOCKET, SO_LINGER,
                       (const char *)&ling, sizeof(ling));
        }
        c->is_closing = 1;
    }

    if (ev == MG_EV_CONNECT) {
        int rc = 0;
        if (ev_data) rc = *(int *)ev_data;
        wisp_logf("http | MG_EV_CONNECT rc=%d", rc);
        if (rc != 0) {
            ctx->error_seen = 1;
            ctx->closed = 1;
            ctx->finished = 1;
            return;
        }
        if (mg_url_is_ssl(ctx->url)) {
            struct mg_tls_opts tls_opts;
            memset(&tls_opts, 0, sizeof(tls_opts));
            tls_opts.ca   = mg_str("");
            tls_opts.name = mg_url_host(ctx->url);
            mg_tls_init(c, &tls_opts);
            wisp_logf("http | tls_init done, is_tls_hs=%d",
                       c->is_tls_hs);
        }
        return;
    }

    if (ev == MG_EV_TLS_HS) {
        wisp_logf("http | MG_EV_TLS_HS");
        return;
    }

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        wisp_logf("http | MG_EV_HTTP_MSG head=%zu body=%zu",
                   hm->head.len, hm->body.len);

        /* Raw body dump — safe inside the callback where c is valid. */
        if (hm->body.len > 0 && hm->body.buf) {
            size_t dump = hm->body.len > 4096 ? 4096 : hm->body.len;
            wisp_logf("http | body (first %zu of %zu):",
                       dump, hm->body.len);
            wisp_log_raw(hm->body.buf, dump);
            wisp_logf("http | --- end body ---");
        }

        r->status = 0;
        if (hm->head.len >= 12) {
            const char *sp = hm->head.buf + 8;
            if (sp < hm->head.buf + hm->head.len) {
                r->status = atoi(sp);
            }
        }

        /* Some responses still carry chunked framing bytes when
         * mongoose hands us the body. Run it through dechunk so the
         * JSON we parse is clean. */
        char *dec = NULL;
        size_t dlen = 0;
        if (dechunk(hm->body.buf, hm->body.len, &dec, &dlen) == 1) {
            finalize_body(ctx, dec, dlen);
            free(dec);
        } else {
            finalize_body(ctx, hm->body.buf, hm->body.len);
        }

        ctx->http_msg_seen = 1;
        ctx->finished = 1;
        c->is_closing = 1;
        return;
    }

    if (ev == MG_EV_ERROR) {
        wisp_logf("http | MG_EV_ERROR %s",
                   ev_data ? (const char *)ev_data : "(no msg)");
        ctx->error_seen = 1;
        ctx->closed = 1;
        ctx->finished = 1;
        return;
    }
    if (ev == MG_EV_CLOSE) {
        wisp_logf("http | MG_EV_CLOSE");
        ctx->closed = 1;
        ctx->finished = 1;
        return;
    }
}

static int dechunk(const char *body, size_t body_len,
                   char **out, size_t *out_len) {
    size_t cap = body_len + 1;
    char  *buf = malloc(cap);
    if (!buf) return -1;
    size_t w = 0;

    const char *p   = body;
    const char *end = body + body_len;

    while (p < end) {
        char *size_end = NULL;
        unsigned long sz = strtoul(p, &size_end, 16);
        if (size_end == p) { free(buf); return -1; }

        const char *nl = memchr(size_end, '\n', (size_t)(end - size_end));
        if (!nl) { free(buf); return 0; }
        p = nl + 1;

        if (sz == 0) {
            while (p < end) {
                const char *tnl = memchr(p, '\n', (size_t)(end - p));
                if (!tnl) break;
                size_t llen = (size_t)(tnl - p);
                if (llen == 0 || (llen == 1 && p[0] == '\r')) {
                    p = tnl + 1;
                    break;
                }
                p = tnl + 1;
            }
            *out = buf;
            *out_len = w;
            return 1;
        }

        if (p + sz > end) { free(buf); return 0; }
        memcpy(buf + w, p, sz);
        w += sz;
        p += sz;

        if (p + 1 < end && p[0] == '\r' && p[1] == '\n') p += 2;
        else if (p < end && p[0] == '\n') p += 1;
    }

    free(buf);
    return 0;
}


int http_post_json(const char *body, http_reply_t *out, int timeout_ms) {
    memset(out, 0, sizeof(*out));

    char url[512];
    snprintf(url, sizeof(url), "%s", settings_api_url());
    char api_key[256];
    snprintf(api_key, sizeof(api_key), "%s", settings_api_key());

    wisp_logf("http | POST start, body=%zu bytes, timeout=%d",
               strlen(body), timeout_ms);

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    wisp_logf("http | url %s", url);
    wisp_logf("http | is_ssl=%d", (int) mg_url_is_ssl(url));

    http_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.r = out;
    ctx.url = url;

    struct mg_connection *c = mg_http_connect(&mgr, url, http_cb, &ctx);
    if (!c) {
        wisp_logf("http | mg_http_connect returned NULL");
        mg_mgr_free(&mgr);
        return -1;
    }

    /* Build headers. We still hold c here — it's only valid until
     * the first mg_mgr_poll. */
    const char *host_begin = strstr(url, "://");
    host_begin = host_begin ? host_begin + 3 : url;
    const char *host_end = strchr(host_begin, '/');
    size_t host_len = host_end ? (size_t)(host_end - host_begin)
                               : strlen(host_begin);
    char host[256];
    if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
    memcpy(host, host_begin, host_len);
    host[host_len] = '\0';

    const char *path = host_end ? host_end : "/";

    char hdrs[2048];
    int hn = snprintf(hdrs, sizeof(hdrs),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "%s%s%s"
        "%s%s%s"
        "%s%s%s"
        "\r\n",
        path, host, strlen(body),
        api_key[0]            ? "Authorization: Bearer " : "",
        api_key[0]            ? api_key : "",
        api_key[0]            ? "\r\n" : "",
        WISP_HTTP_REFERER[0]  ? "HTTP-Referer: " : "",
        WISP_HTTP_REFERER[0]  ? WISP_HTTP_REFERER : "",
        WISP_HTTP_REFERER[0]  ? "\r\n" : "",
        WISP_X_TITLE[0]       ? "X-Title: " : "",
        WISP_X_TITLE[0]       ? WISP_X_TITLE : "",
        WISP_X_TITLE[0]       ? "\r\n" : "");

    if (hn < 0 || hn >= (int)sizeof(hdrs)) {
        wisp_logf("http | header buffer too small (need %d)", hn);
        mg_mgr_free(&mgr);
        return -1;
    }

    mg_send(c, hdrs, (size_t)hn);
    mg_send(c, body, strlen(body));

    /* From here on, never touch c. mg_mgr_poll may free it at any
     * time. All state we need is in `ctx`, which outlives the
     * connection. */
    uint64_t deadline = mg_millis() + (uint64_t)timeout_ms;
    int finalized = 0;
    int cancelled = 0;

    while (mg_millis() < deadline && !g_quit) {
        mg_mgr_poll(&mgr, 50);
        if (out->body)   { finalized = 1; break; }
        if (ctx.finished) break;

        if (g_cancel_request) {
            cancelled = 1;
            ctx.cancel_requested = 1;

            /* Pump the manager until the connection actually closes,
             * or until we give up. A normal TCP close completes in a
             * few ms; the 500 ms ceiling is just a safety net. */
            uint64_t close_deadline = mg_millis() + 500;
            while (mg_millis() < close_deadline && !ctx.closed) {
                mg_mgr_poll(&mgr, 20);
            }
            break;
        }
    }

    wisp_logf("http | loop end: status=%d finalized=%d msg=%d err=%d cancel=%d",
              out->status, finalized, ctx.http_msg_seen,
              ctx.error_seen, cancelled);

    mg_mgr_free(&mgr);

    if (cancelled) {
        g_cancel_request = 0;             /* consumed */
        free(out->body);
        out->body = NULL;
        out->len = 0;
        if (out->status == 0) out->status = -1;
        return -2;                        /* distinct cancel code */
    }

    /* Request finished on its own. If the user pressed ESC at the last
     * instant, the reply already won the race — discard the stale flag
     * so the caller doesn't throw away a valid response. */
    g_cancel_request = 0;

    if (!finalized || !out->body) {
        if (out->status == 0) out->status = -1;
        return -1;
    }

    return 0;
}

int tcp_port_open(const char *host, int port, int timeout_ms) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);

    if (getaddrinfo(host, portbuf, &hints, &res) != 0) return 0;

    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
#if defined(_WIN32)
        SOCKET fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == INVALID_SOCKET) continue;
#else
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
#endif
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        int ok = (connect(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0);
#if defined(_WIN32)
        closesocket(fd);
#else
        close(fd);
#endif
        if (ok) { freeaddrinfo(res); return 1; }
    }
    if (res) freeaddrinfo(res);
    return 0;
}

static void project_session_id(char *out, size_t out_sz) {
    unsigned long long h = 5381;
    for (const char *p = g_project.root; *p; p++) {
        h = ((h << 5) + h) ^ (unsigned char)*p;
    }
    h ^= (unsigned long long)wisp_getpid() * 0x9e3779b97f4a7c15ULL;
    snprintf(out, out_sz, "wisp-%016llx", h);
}

/* ================================================================== */
/*  Request body                                                      */
/* ================================================================== */

char *build_request_body(const char *user_prompt) {
    size_t changes_cap = 8 * 1024 * 1024;
    char *changes = malloc(changes_cap);
    if (!changes) return NULL;

    if (g_shell_mode) {
        snapshot_emit_reads(changes, changes_cap);
    } else {
        snapshot_emit_delta(changes, changes_cap);
    }

    char run_block[16384];
    run_block[0] = '\0';
    const char *run_out = "";
    if (g_pending_run_output && g_pending_run_output[0]) {
        snprintf(run_block, sizeof(run_block),
                 "[RUN OUTPUT]\n%s\n[END RUN OUTPUT]\n\n",
                 g_pending_run_output);
        run_out = run_block;
        free(g_pending_run_output);
        g_pending_run_output = NULL;
    }

    char *snapshot_block = NULL;
    const char *snap = "";
    if (!g_shell_mode && !g_project.last_sent_valid) {
        size_t snap_len = g_project.snapshot ? g_project.snapshot_len : 0;
        snapshot_block = malloc(snap_len + 128);
        if (snapshot_block) {
            snprintf(snapshot_block, snap_len + 128,
                     "--- PROJECT SNAPSHOT ---\n"
                     "%s\n"
                     "--- END SNAPSHOT ---\n\n",
                     g_project.snapshot ? g_project.snapshot : "(empty)");
            snap = snapshot_block;
        }
    }

    size_t changes_len = strlen(changes);
    size_t run_len     = strlen(run_out);
    size_t snap_len    = strlen(snap);
    size_t prompt_len  = strlen(user_prompt);

    size_t need = changes_len + run_len + snap_len + prompt_len + 16;
    char *turn = malloc(need);
    if (!turn) {
        free(changes);
        free(snapshot_block);
        return NULL;
    }

    int n = snprintf(turn, need, "%s%s%s%s",
                     snap,
                     changes_len ? changes : "",
                     run_out,
                     user_prompt);
    free(changes);
    free(snapshot_block);
    if (n < 0) { free(turn); return NULL; }

    history_append_user(turn);
    free(turn);

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "model", settings_model());

    cJSON *messages = NULL;
    if (endpoint_is_relay()) {
        if (!g_project.last_sent_valid) {
            messages = history_to_json_pair();
        } else {
            messages = history_to_json_last();
        }
    } else {
        messages = history_to_json();
    }
    if (!messages) { cJSON_Delete(root); return NULL; }

    cJSON_AddItemToObject(root, "messages", messages);

    cJSON_AddNumberToObject(root, "temperature", 0.6);

    cJSON_AddBoolToObject(root, "stream", 0);

    if (!endpoint_is_relay()) {
        cJSON *reasoning = cJSON_CreateObject();
        cJSON_AddBoolToObject(reasoning, "exclude", 0);
        cJSON_AddItemToObject(root, "reasoning", reasoning);
    }

    char sid[64];
    project_session_id(sid, sizeof(sid));

    cJSON_AddStringToObject(root, "session_id", sid);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}


char *extract_content(const char *reply_json) {
    cJSON *root = cJSON_Parse(reply_json);
    if (!root) return NULL;
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    char *out = NULL;
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *c0 = cJSON_GetArrayItem(choices, 0);
        cJSON *m  = cJSON_GetObjectItem(c0, "message");
        cJSON *ct = cJSON_GetObjectItem(m, "content");
        if (cJSON_IsString(ct)) out = strdup(ct->valuestring);
    }
    cJSON_Delete(root);
    return out;
}

/* GET request for a URL. */
char *http_get_text(const char *url, const char *bearer_token,
                    char *err_out, size_t err_sz) {
    if (err_out && err_sz) err_out[0] = '\0';

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    http_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    http_reply_t r = {0};
    ctx.r = &r;
    ctx.url = url;

    struct mg_connection *c = mg_http_connect(&mgr, url, http_cb, &ctx);
    if (!c) {
        mg_mgr_free(&mgr);
        if (err_out) snprintf(err_out, err_sz, "connect failed");
        return NULL;
    }

    const char *host_begin = strstr(url, "://");
    host_begin = host_begin ? host_begin + 3 : url;
    const char *host_end = strchr(host_begin, '/');
    size_t host_len = host_end ? (size_t)(host_end - host_begin) : strlen(host_begin);
    char host[256];
    if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
    memcpy(host, host_begin, host_len);
    host[host_len] = '\0';
    const char *path = host_end ? host_end : "/";

    char hdrs[1024];
    int hn = snprintf(hdrs, sizeof(hdrs),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "%s%s%s"
        "\r\n",
        path, host,
        (bearer_token && bearer_token[0]) ? "Authorization: Bearer " : "",
        (bearer_token && bearer_token[0]) ? bearer_token : "",
        (bearer_token && bearer_token[0]) ? "\r\n" : "");

    if (hn < 0 || hn >= (int)sizeof(hdrs)) {
        mg_mgr_free(&mgr);
        if (err_out) snprintf(err_out, err_sz, "header too long");
        return NULL;
    }

    mg_send(c, hdrs, (size_t)hn);

    uint64_t deadline = mg_millis() + 10000;
    int finalized = 0;
    int cancelled = 0;

    while (mg_millis() < deadline && !g_quit) {
        mg_mgr_poll(&mgr, 50);
        if (r.body)      { finalized = 1; break; }
        if (ctx.finished) break;

        if (g_cancel_request) {
            cancelled = 1;
            ctx.cancel_requested = 1;

            uint64_t close_deadline = mg_millis() + 500;
            while (mg_millis() < close_deadline && !ctx.closed) {
                mg_mgr_poll(&mgr, 20);
            }
            break;
        }
    }

    mg_mgr_free(&mgr);

    if (cancelled) {
        g_cancel_request = 0;
        free(r.body);
        if (err_out) snprintf(err_out, err_sz, "cancelled");
        return NULL;
    }

    if (!finalized || !r.body) {
        if (err_out) snprintf(err_out, err_sz, "no response");
        free(r.body);
        return NULL;
    }
    
    if (r.status >= 400) {
        if (err_out) snprintf(err_out, err_sz, "HTTP %d", r.status);
        free(r.body);
        return NULL;
    }

    return r.body;
}