/** Copyright (c) 2026-2036, Jin.Wu <wujin.developer@gmail.com>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 */

#include "http-common.h"
#include "http-transport.h"
#include "llhttp.h"

#include "xylem/http/xylem-http-client.h"
#include "xylem/xylem-addr.h"
#include "xylem/xylem-gzip.h"
#include "xylem/xylem-loop.h"
#include "xylem/xylem-thrdpool.h"
#include "xylem/xylem-utils.h"
#include "xylem/xylem-xlist.h"
#include "xylem/xylem-xrbtree.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct xylem_http_res_s {
    int            status_code;
    http_header_t* headers;
    size_t         header_count;
    size_t         header_cap;
    uint8_t*       body;
    size_t         body_len;
};

typedef struct {
    xylem_loop_t*              loop;
    xylem_thrdpool_t*          pool;
    xylem_loop_timer_t*        timeout_timer;
    http_url_t                 url;
    const char*                method;
    const void*                body;
    size_t                     body_len;
    const char*                content_type;
    xylem_http_res_t*      res;
    llhttp_t                   parser;
    llhttp_settings_t          settings;
    char*                      cur_header_name;
    size_t                     cur_header_name_len;
    const http_transport_vt_t* vt;
    void*                      conn;
    http_transport_cb_t        transport_cb;
    bool                       timed_out;
    bool                       done;
    bool                       expect_continue;
    bool                       continue_received;
    int                        redirects_remaining;
    size_t                     max_body_size;
    const xylem_http_hdr_t*    custom_headers;
    size_t                     custom_header_count;
    xylem_http_cookie_jar_t*   cookie_jar;
    const char*                range;
    xylem_http_hdr_t*          merged_headers;
    size_t                     merged_header_count;
    bool                       is_session;
} _http_cli_ctx_t;

#define DEFAULT_TIMEOUT_MS       30000
#define DEFAULT_MAX_BODY         10485760 /* 10 MiB */
#define DEFAULT_MAX_IDLE         5
#define DEFAULT_IDLE_TIMEOUT_MS  90000

typedef struct {
    const http_transport_vt_t* vt;
    void*                      transport;
    uint64_t                   idle_since_ms;
} _http_session_idle_conn_t;

typedef struct {
    char          key[320];
    xylem_xlist_t idle_conns;
} _http_session_pool_entry_t;

struct xylem_http_session_s {
    xylem_loop_t*            loop;
    xylem_thrdpool_t*        pool;
    xylem_xrbtree_t          conn_pool;
    size_t                   max_idle_per_host;
    uint64_t                 idle_timeout_ms;
    xylem_http_cookie_jar_t* cookie_jar;
};

static int _http_session_pool_cmp_dd(const void* a, const void* b) {
    const _http_session_pool_entry_t* ea = a;
    const _http_session_pool_entry_t* eb = b;
    return strcmp(ea->key, eb->key);
}

static int _http_session_pool_cmp_kd(const void* key, const void* data) {
    const char* k = key;
    const _http_session_pool_entry_t* e = data;
    return strcmp(k, e->key);
}

static void _http_session_make_key(const http_url_t* url,
                                   char* buf, size_t buf_size) {
    snprintf(buf, buf_size, "%s:%u:%s",
             url->host, (unsigned)url->port, url->scheme);
}

typedef struct {
    char*    name;
    char*    value;
    char*    domain;
    char*    path;
    uint64_t expires;   /* seconds since epoch, 0 = session cookie */
    bool     secure;
    bool     http_only;
} _cookie_t;

struct xylem_http_cookie_jar_s {
    _cookie_t* cookies;
    size_t     count;
    size_t     cap;
};

static char* _http_cli_cookie_strdup(const char* s, size_t len) {
    char* d = malloc(len + 1);
    if (!d) {
        return NULL;
    }
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

static void _http_cli_cookie_free(_cookie_t* c) {
    free(c->name);
    free(c->value);
    free(c->domain);
    free(c->path);
}

/* Skip leading whitespace. */
static const char* _http_cli_skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/* Case-insensitive prefix match using lookup table. */
static bool _http_cli_iprefix(const char* s, const char* prefix, size_t plen) {
    for (size_t i = 0; i < plen; i++) {
        if (http_lower_table[(uint8_t)s[i]] !=
            http_lower_table[(uint8_t)prefix[i]]) {
            return false;
        }
    }
    return true;
}

/**
 * Parse a Set-Cookie header value into a _cookie_t.
 * Returns 0 on success, -1 on parse error.
 */
static int _http_cli_cookie_parse(const char* header, const char* req_host,
                                  const char* req_path, _cookie_t* out) {
    memset(out, 0, sizeof(*out));

    /* name=value is before the first ';' */
    const char* semi = strchr(header, ';');
    size_t nv_len = semi ? (size_t)(semi - header) : strlen(header);

    const char* eq = memchr(header, '=', nv_len);
    if (!eq || eq == header) {
        return -1;
    }

    size_t name_len = (size_t)(eq - header);
    size_t val_len  = nv_len - name_len - 1;

    out->name  = _http_cli_cookie_strdup(header, name_len);
    out->value = _http_cli_cookie_strdup(eq + 1, val_len);
    if (!out->name || !out->value) {
        _http_cli_cookie_free(out);
        return -1;
    }

    /* Default domain and path from request. */
    out->domain = _http_cli_cookie_strdup(req_host, strlen(req_host));
    out->path   = _http_cli_cookie_strdup("/", 1);
    if (!out->domain || !out->path) {
        _http_cli_cookie_free(out);
        return -1;
    }

    /* Parse attributes. */
    const char* p = semi ? semi + 1 : NULL;
    while (p && *p) {
        p = _http_cli_skip_ws(p);
        const char* next = strchr(p, ';');
        size_t attr_len = next ? (size_t)(next - p) : strlen(p);

        const char* aeq = memchr(p, '=', attr_len);
        size_t key_len = aeq ? (size_t)(aeq - p) : attr_len;

        /* Trim trailing whitespace from key. */
        while (key_len > 0 && (p[key_len - 1] == ' ' || p[key_len - 1] == '\t')) {
            key_len--;
        }

        if (key_len == 6 && aeq &&
            _http_cli_iprefix(p, "domain", 6)) {
            const char* v = _http_cli_skip_ws(aeq + 1);
            size_t vlen = attr_len - (size_t)(v - p);
            /* Strip leading dot. */
            if (vlen > 0 && v[0] == '.') {
                v++;
                vlen--;
            }
            free(out->domain);
            out->domain = _http_cli_cookie_strdup(v, vlen);
        } else if (key_len == 4 && aeq &&
                   _http_cli_iprefix(p, "path", 4)) {
            const char* v = _http_cli_skip_ws(aeq + 1);
            size_t vlen = attr_len - (size_t)(v - p);
            free(out->path);
            out->path = _http_cli_cookie_strdup(v, vlen);
        } else if (key_len == 7 && aeq &&
                   _http_cli_iprefix(p, "max-age", 7)) {
            const char* v = _http_cli_skip_ws(aeq + 1);
            long age = strtol(v, NULL, 10);
            if (age <= 0) {
                out->expires = 1; /* expired */
            } else {
                out->expires = xylem_utils_getnow(XYLEM_TIME_PRECISION_SEC)
                             + (uint64_t)age;
            }
        } else if (key_len == 6 &&
                   _http_cli_iprefix(p, "secure", 6)) {
            out->secure = true;
        } else if (key_len == 8 &&
                   _http_cli_iprefix(p, "httponly", 8)) {
            out->http_only = true;
        }
        /* Expires attribute intentionally not parsed -- Max-Age takes precedence
         * and parsing HTTP-date is complex. Session cookies (expires=0) are the
         * default when neither Max-Age nor Expires is present. */

        p = next ? next + 1 : NULL;
    }

    (void)req_path;
    return 0;
}

/* Domain tail match: cookie domain "example.com" matches "sub.example.com". */
static bool _http_cli_cookie_domain_match(const char* cookie_domain,
                                          const char* req_host) {
    size_t cd_len = strlen(cookie_domain);
    size_t rh_len = strlen(req_host);

    if (cd_len == rh_len) {
        return http_header_eq(cookie_domain, req_host);
    }
    if (cd_len < rh_len) {
        size_t offset = rh_len - cd_len;
        if (req_host[offset - 1] != '.') {
            return false;
        }
        return http_header_eq(cookie_domain, req_host + offset);
    }
    return false;
}

/* Path prefix match: cookie path "/foo" matches "/foo/bar". */
static bool _http_cli_cookie_path_match(const char* cookie_path,
                                        const char* req_path) {
    size_t cp_len = strlen(cookie_path);
    size_t rp_len = strlen(req_path);

    if (rp_len < cp_len) {
        return false;
    }
    if (memcmp(cookie_path, req_path, cp_len) != 0) {
        return false;
    }
    if (rp_len == cp_len) {
        return true;
    }
    /* Cookie path "/foo" matches "/foo/bar" but not "/foobar". */
    if (cookie_path[cp_len - 1] == '/') {
        return true;
    }
    return req_path[cp_len] == '/';
}

static bool _http_cli_cookie_match(const _cookie_t* c, const char* scheme,
                                   const char* host, const char* path) {
    /* Check expiry. */
    if (c->expires > 0) {
        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_SEC);
        if (now >= c->expires) {
            return false;
        }
    }

    /* Secure cookies only over HTTPS. */
    if (c->secure && strcmp(scheme, "https") != 0) {
        return false;
    }

    if (!_http_cli_cookie_domain_match(c->domain, host)) {
        return false;
    }

    if (!_http_cli_cookie_path_match(c->path, path)) {
        return false;
    }

    return true;
}

/* Store a cookie in the jar, replacing any existing cookie with same name+domain+path. */
static void _http_cli_cookie_jar_store(xylem_http_cookie_jar_t* jar,
                                       _cookie_t* c) {
    /* Look for existing cookie to replace. */
    for (size_t i = 0; i < jar->count; i++) {
        _cookie_t* existing = &jar->cookies[i];
        if (strcmp(existing->name, c->name) == 0 &&
            http_header_eq(existing->domain, c->domain) &&
            strcmp(existing->path, c->path) == 0) {
            _http_cli_cookie_free(existing);
            *existing = *c;
            return;
        }
    }

    /* Grow array if needed. */
    if (jar->count >= jar->cap) {
        size_t new_cap = jar->cap ? jar->cap * 2 : 8;
        _cookie_t* tmp = realloc(jar->cookies, new_cap * sizeof(*tmp));
        if (!tmp) {
            _http_cli_cookie_free(c);
            return;
        }
        jar->cookies = tmp;
        jar->cap = new_cap;
    }

    jar->cookies[jar->count++] = *c;
}

/**
 * Collect Set-Cookie headers from response and store in jar.
 */
static void _http_cli_cookie_jar_collect(xylem_http_cookie_jar_t* jar,
                                         const http_header_t* headers,
                                         size_t header_count,
                                         const char* req_host,
                                         const char* req_path) {
    for (size_t i = 0; i < header_count; i++) {
        /* "Set-Cookie" is 10 chars; skip length-mismatched names. */
        if (strlen(headers[i].name) != 10 ||
            !http_header_eq(headers[i].name, "Set-Cookie")) {
            continue;
        }
        _cookie_t c;
        if (_http_cli_cookie_parse(headers[i].value, req_host, req_path,
                                   &c) == 0) {
            _http_cli_cookie_jar_store(jar, &c);
        }
    }
}

/**
 * Build a "Cookie: name=val; name2=val2" header value from matching cookies.
 * Returns malloc'd string or NULL if no cookies match.
 */
static char* _http_cli_cookie_jar_build(const xylem_http_cookie_jar_t* jar,
                                        const char* scheme, const char* host,
                                        const char* path) {
    size_t cap = 128;
    char* buf = malloc(cap);
    if (!buf) {
        return NULL;
    }

    size_t off = 0;
    bool first = true;
    for (size_t i = 0; i < jar->count; i++) {
        if (!_http_cli_cookie_match(&jar->cookies[i], scheme, host, path)) {
            continue;
        }

        size_t nlen = strlen(jar->cookies[i].name);
        size_t vlen = strlen(jar->cookies[i].value);
        /* "name=value" plus "; " separator */
        size_t need = nlen + 1 + vlen + (first ? 0 : 2);

        if (off + need + 1 > cap) {
            while (off + need + 1 > cap) {
                cap *= 2;
            }
            char* tmp = realloc(buf, cap);
            if (!tmp) {
                free(buf);
                return NULL;
            }
            buf = tmp;
        }

        if (!first) {
            buf[off++] = ';';
            buf[off++] = ' ';
        }
        memcpy(buf + off, jar->cookies[i].name, nlen);
        off += nlen;
        buf[off++] = '=';
        memcpy(buf + off, jar->cookies[i].value, vlen);
        off += vlen;
        first = false;
    }

    if (first) {
        free(buf);
        return NULL;
    }

    buf[off] = '\0';
    return buf;
}

/* Session-specific abort: stop timer and loop but do NOT close conn. */
static void _http_session_abort(_http_cli_ctx_t* ctx) {
    xylem_loop_stop_timer(ctx->timeout_timer);
    xylem_loop_destroy_timer(ctx->timeout_timer);
    xylem_loop_stop(ctx->loop);
}

/* Stop and close the timeout timer, then stop the event loop. */
static void _http_cli_abort(_http_cli_ctx_t* ctx) {
    xylem_loop_stop_timer(ctx->timeout_timer);
    xylem_loop_destroy_timer(ctx->timeout_timer);
    xylem_loop_stop(ctx->loop);
}

static int _http_cli_res_header_field_cb(llhttp_t* parser,
                                         const char* at, size_t len) {
    _http_cli_ctx_t* ctx = parser->data;

    free(ctx->cur_header_name);
    ctx->cur_header_name = malloc(len + 1);
    if (!ctx->cur_header_name) {
        return HPE_USER;
    }
    memcpy(ctx->cur_header_name, at, len);
    ctx->cur_header_name[len] = '\0';
    ctx->cur_header_name_len = len;
    return 0;
}

static int _http_cli_res_header_value_cb(llhttp_t* parser,
                                         const char* at, size_t len) {
    _http_cli_ctx_t* ctx = parser->data;
    if (!ctx->cur_header_name || !ctx->res) {
        return 0;
    }

    if (http_header_add(&ctx->res->headers, &ctx->res->header_count,
                        &ctx->res->header_cap,
                        ctx->cur_header_name, ctx->cur_header_name_len,
                        at, len) != 0) {
        return HPE_USER;
    }

    free(ctx->cur_header_name);
    ctx->cur_header_name = NULL;
    ctx->cur_header_name_len = 0;
    return 0;
}

static int _http_cli_res_headers_complete_cb(llhttp_t* parser) {
    _http_cli_ctx_t* ctx = parser->data;
    if (ctx->res) {
        ctx->res->status_code = (int)parser->status_code;

        /* Pre-allocate body buffer if Content-Length is known. */
        uint64_t content_length = parser->content_length;
        if (content_length > 0 && content_length != ULLONG_MAX &&
            content_length <= ctx->max_body_size) {
            ctx->res->body = malloc((size_t)content_length);
            /* Allocation failure is non-fatal; realloc path handles it. */
        }
    }

    if (parser->status_code == 100) {
        ctx->continue_received = true;
    }

    return 0;
}

static int _http_cli_res_body_cb(llhttp_t* parser,
                                 const char* at, size_t len) {
    _http_cli_ctx_t* ctx = parser->data;
    if (!ctx->res) {
        return 0;
    }

    if (ctx->res->body_len + len > ctx->max_body_size) {
        return HPE_USER;
    }

    /* If body was pre-allocated from Content-Length, just memcpy. */
    uint64_t content_length = parser->content_length;
    if (ctx->res->body && content_length != ULLONG_MAX &&
        ctx->res->body_len + len <= (size_t)content_length) {
        memcpy(ctx->res->body + ctx->res->body_len, at, len);
        ctx->res->body_len += len;
        return 0;
    }

    uint8_t* tmp = realloc(ctx->res->body, ctx->res->body_len + len);
    if (!tmp) {
        return HPE_USER;
    }
    memcpy(tmp + ctx->res->body_len, at, len);
    ctx->res->body = tmp;
    ctx->res->body_len += len;
    return 0;
}

static int _http_cli_res_message_complete_cb(llhttp_t* parser) {
    _http_cli_ctx_t* ctx = parser->data;

    /* Auto-decompress gzip response body (RFC 9110 section 8.4). */
    if (ctx->res && ctx->res->body && ctx->res->body_len > 0) {
        const char* ce = http_header_find(ctx->res->headers,
                                          ctx->res->header_count,
                                          "Content-Encoding");
        if (ce && http_header_eq(ce, "gzip")) {
            /* Try progressively larger buffers (4x, 8x, 16x). */
            size_t src_len = ctx->res->body_len;
            int rc = -1;
            size_t mult = 4;
            uint8_t* dec = NULL;
            while (mult <= 16) {
                size_t dec_cap = src_len * mult;
                dec = malloc(dec_cap);
                if (!dec) {
                    break;
                }
                rc = xylem_gzip_decompress(ctx->res->body, src_len,
                                           dec, dec_cap);
                if (rc >= 0) {
                    break;
                }
                free(dec);
                dec = NULL;
                mult *= 2;
            }
            if (rc >= 0 && dec) {
                free(ctx->res->body);
                ctx->res->body = dec;
                ctx->res->body_len = (size_t)rc;
            } else {
                free(dec);
            }
        }
    }

    ctx->done = true;
    return HPE_PAUSED;
}

static void _http_cli_timeout_cb(xylem_loop_t* loop,
                                 xylem_loop_timer_t* timer,
                                 void* ud) {
    (void)loop;
    (void)timer;
    _http_cli_ctx_t* ctx = ud;
    ctx->timed_out = true;
    if (ctx->conn) {
        ctx->vt->close_conn(ctx->conn);
        ctx->conn = NULL;
    }
}

static void _http_cli_conn_read_cb(void* handle, void* user,
                                   void* data, size_t len) {
    (void)handle;
    _http_cli_ctx_t* ctx = user;

    enum llhttp_errno err = llhttp_execute(&ctx->parser, data, len);

    if (ctx->done) {
        if (ctx->conn && !ctx->is_session) {
            ctx->vt->close_conn(ctx->conn);
            ctx->conn = NULL;
        }
        if (ctx->is_session) {
            _http_session_abort(ctx);
        }
        return;
    }

    /**
     * 100 Continue received: send the body now.
     * Reset parser to continue reading the real response.
     */
    if (ctx->continue_received && ctx->expect_continue) {
        ctx->expect_continue = false;
        if (ctx->body && ctx->body_len > 0 && ctx->conn) {
            ctx->vt->send(ctx->conn, ctx->body, ctx->body_len);
        }
        llhttp_resume(&ctx->parser);
        return;
    }

    if (err != HPE_OK && err != HPE_PAUSED) {
        if (ctx->conn) {
            ctx->vt->close_conn(ctx->conn);
            ctx->conn = NULL;
        }
    }
}

static void _http_cli_conn_connect_cb(void* handle, void* user) {
    (void)handle;
    _http_cli_ctx_t* ctx = user;

    bool use_continue = (ctx->body_len > 1024);
    ctx->expect_continue = use_continue;

    /* Build merged headers: custom headers + Cookie header from jar. */
    free(ctx->merged_headers);
    ctx->merged_headers = NULL;
    ctx->merged_header_count = 0;

    char* cookie_val = NULL;
    const xylem_http_hdr_t* hdrs = ctx->custom_headers;
    size_t hdr_count = ctx->custom_header_count;

    if (ctx->cookie_jar) {
        cookie_val = _http_cli_cookie_jar_build(ctx->cookie_jar,
                                                ctx->url.scheme,
                                                ctx->url.host,
                                                ctx->url.path);
    }

    /* Count extra headers to inject (Cookie, Range). */
    size_t extra = 0;
    if (cookie_val) {
        extra++;
    }
    if (ctx->range) {
        extra++;
    }

    if (extra > 0) {
        ctx->merged_header_count = ctx->custom_header_count + extra;
        ctx->merged_headers = malloc(ctx->merged_header_count
                                     * sizeof(xylem_http_hdr_t));
        if (ctx->merged_headers) {
            for (size_t i = 0; i < ctx->custom_header_count; i++) {
                ctx->merged_headers[i] = ctx->custom_headers[i];
            }
            size_t idx = ctx->custom_header_count;
            if (cookie_val) {
                ctx->merged_headers[idx].name  = "Cookie";
                ctx->merged_headers[idx].value = cookie_val;
                idx++;
            }
            if (ctx->range) {
                ctx->merged_headers[idx].name  = "Range";
                ctx->merged_headers[idx].value = ctx->range;
                idx++;
            }
            hdrs = ctx->merged_headers;
            hdr_count = ctx->merged_header_count;
        }
    }

    size_t req_len;
    char* req_buf = http_req_serialize(ctx->method, &ctx->url,
                                       ctx->body, ctx->body_len,
                                       ctx->content_type,
                                       use_continue, &req_len,
                                       hdrs, hdr_count);
    free(cookie_val);

    if (!req_buf) {
        if (ctx->conn) {
            ctx->vt->close_conn(ctx->conn);
            ctx->conn = NULL;
        }
        return;
    }

    ctx->vt->send(ctx->conn, req_buf, req_len);
    free(req_buf);
}

static void _http_cli_conn_close_cb(void* handle, void* user, int err) {
    (void)handle;
    (void)err;
    _http_cli_ctx_t* ctx = user;
    ctx->conn = NULL;
    if (ctx->is_session) {
        _http_session_abort(ctx);
    } else {
        _http_cli_abort(ctx);
    }
}

static void _http_cli_resolve_cb(xylem_addr_t* addrs, size_t count,
                                 int status, void* userdata) {
    _http_cli_ctx_t* ctx = userdata;

    if (status != 0 || count == 0) {
        _http_cli_abort(ctx);
        return;
    }

    if (strcmp(ctx->url.scheme, "https") == 0) {
        ctx->vt = http_transport_tls();
        if (!ctx->vt) {
            _http_cli_abort(ctx);
            return;
        }
    } else {
        ctx->vt = http_transport_tcp();
    }

    ctx->transport_cb.on_connect    = _http_cli_conn_connect_cb;
    ctx->transport_cb.on_read       = _http_cli_conn_read_cb;
    ctx->transport_cb.on_close      = _http_cli_conn_close_cb;
    ctx->transport_cb.on_write_done = NULL;
    ctx->transport_cb.on_accept     = NULL;

    ctx->conn = ctx->vt->dial(ctx->loop, &addrs[0],
                              &ctx->transport_cb, ctx, NULL);
    if (!ctx->conn) {
        _http_cli_abort(ctx);
    }
}


/**
 * Check whether the response is a redirect that should be followed.
 * On success, updates ctx->url and ctx->method for the next iteration,
 * destroys the current response, and returns true.
 */
static bool _http_cli_follow_redirect(_http_cli_ctx_t* ctx) {
    int status = ctx->res->status_code;
    if (ctx->redirects_remaining <= 0 ||
        (status != 301 && status != 302 && status != 303 &&
         status != 307 && status != 308)) {
        return false;
    }

    const char* location = http_header_find(
        ctx->res->headers, ctx->res->header_count, "Location");
    if (!location) {
        return false;
    }

    http_url_t new_url;
    int parse_rc;

    if (strstr(location, "://")) {
        parse_rc = http_url_parse(location, &new_url);
    } else {
        /**
         * Relative redirect: keep scheme/host/port,
         * replace path.
         */
        new_url = ctx->url;
        size_t loc_len = strlen(location);
        if (loc_len >= sizeof(new_url.path)) {
            parse_rc = -1;
        } else {
            memcpy(new_url.path, location, loc_len);
            new_url.path[loc_len] = '\0';
            parse_rc = 0;
        }
    }

    if (parse_rc != 0) {
        return false;
    }

    xylem_http_res_destroy(ctx->res);
    ctx->res = NULL;
    ctx->url = new_url;
    ctx->redirects_remaining--;

    if (status == 301 || status == 302 || status == 303) {
        ctx->method       = "GET";
        ctx->body         = NULL;
        ctx->body_len     = 0;
        ctx->content_type = NULL;
    }

    return true;
}

static xylem_http_res_t* _http_cli_exec(const char* method,
                                            const char* url,
                                            const void* body,
                                            size_t body_len,
                                            const char* content_type,
                                            const xylem_http_cli_opts_t* opts) {
    if (!method || !url) {
        return NULL;
    }

    uint64_t timeout_ms    = (opts && opts->timeout_ms)    ? opts->timeout_ms    : DEFAULT_TIMEOUT_MS;
    int      max_redirects = (opts)                        ? opts->max_redirects  : 0;
    size_t   max_body_size = (opts && opts->max_body_size) ? opts->max_body_size  : DEFAULT_MAX_BODY;

    _http_cli_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    if (http_url_parse(url, &ctx.url) != 0) {
        return NULL;
    }

    ctx.method              = method;
    ctx.body                = body;
    ctx.body_len            = body_len;
    ctx.content_type        = content_type;
    ctx.redirects_remaining = max_redirects;
    ctx.max_body_size       = max_body_size;
    ctx.custom_headers      = (opts) ? opts->headers      : NULL;
    ctx.custom_header_count = (opts) ? opts->header_count  : 0;
    ctx.cookie_jar          = (opts) ? opts->cookie_jar    : NULL;
    ctx.range               = (opts) ? opts->range         : NULL;

    ctx.loop = xylem_loop_create();
    if (!ctx.loop) {
        return NULL;
    }

    xylem_thrdpool_t* pool = xylem_thrdpool_create(1);
    if (!pool) {
        xylem_loop_destroy(ctx.loop);
        return NULL;
    }

    for (;;) {
        ctx.done              = false;
        ctx.timed_out         = false;
        ctx.expect_continue   = false;
        ctx.continue_received = false;
        ctx.conn              = NULL;
        ctx.cur_header_name   = NULL;

        ctx.res = calloc(1, sizeof(*ctx.res));
        if (!ctx.res) {
            break;
        }

        llhttp_settings_init(&ctx.settings);
        ctx.settings.on_header_field     = _http_cli_res_header_field_cb;
        ctx.settings.on_header_value     = _http_cli_res_header_value_cb;
        ctx.settings.on_headers_complete = _http_cli_res_headers_complete_cb;
        ctx.settings.on_body             = _http_cli_res_body_cb;
        ctx.settings.on_message_complete = _http_cli_res_message_complete_cb;
        llhttp_init(&ctx.parser, HTTP_RESPONSE, &ctx.settings);
        ctx.parser.data = &ctx;

        ctx.timeout_timer = xylem_loop_create_timer(ctx.loop, &ctx);
        if (timeout_ms > 0) {
            xylem_loop_start_timer(ctx.timeout_timer,
                                   _http_cli_timeout_cb,
                                   timeout_ms, 0);
        }

        xylem_addr_resolve_t* resolve_req =
            xylem_addr_resolve(ctx.loop, pool, ctx.url.host, ctx.url.port,
                               _http_cli_resolve_cb, &ctx);
        if (!resolve_req) {
            _http_cli_abort(&ctx);
            xylem_http_res_destroy(ctx.res);
            ctx.res = NULL;
            break;
        }

        xylem_loop_run(ctx.loop);

        free(ctx.cur_header_name);
        ctx.cur_header_name = NULL;

        if (ctx.timed_out || !ctx.done) {
            xylem_http_res_destroy(ctx.res);
            ctx.res = NULL;
            break;
        }

        /* Collect Set-Cookie headers into jar. */
        if (ctx.cookie_jar && ctx.res) {
            _http_cli_cookie_jar_collect(ctx.cookie_jar,
                                         ctx.res->headers,
                                         ctx.res->header_count,
                                         ctx.url.host, ctx.url.path);
        }

        free(ctx.merged_headers);
        ctx.merged_headers = NULL;
        ctx.merged_header_count = 0;

        if (!_http_cli_follow_redirect(&ctx)) {
            break;
        }
    }

    xylem_loop_destroy(ctx.loop);
    xylem_thrdpool_destroy(pool);
    free(ctx.merged_headers);
    return ctx.res;
}

int xylem_http_res_status(const xylem_http_res_t* res) {
    if (!res) {
        return 0;
    }
    return res->status_code;
}

const char* xylem_http_res_header(const xylem_http_res_t* res,
                                      const char* name) {
    if (!res || !name) {
        return NULL;
    }
    return http_header_find(res->headers, res->header_count, name);
}

const void* xylem_http_res_body(const xylem_http_res_t* res) {
    if (!res) {
        return NULL;
    }
    return res->body;
}

size_t xylem_http_res_body_len(const xylem_http_res_t* res) {
    if (!res) {
        return 0;
    }
    return res->body_len;
}

void xylem_http_res_destroy(xylem_http_res_t* res) {
    if (!res) {
        return;
    }
    http_headers_free(res->headers, res->header_count);
    free(res->body);
    free(res);
}

xylem_http_cookie_jar_t* xylem_http_cookie_jar_create(void) {
    return calloc(1, sizeof(xylem_http_cookie_jar_t));
}

void xylem_http_cookie_jar_destroy(xylem_http_cookie_jar_t* jar) {
    if (!jar) {
        return;
    }
    for (size_t i = 0; i < jar->count; i++) {
        _http_cli_cookie_free(&jar->cookies[i]);
    }
    free(jar->cookies);
    free(jar);
}

xylem_http_res_t* xylem_http_get(const char* url,
                                         const xylem_http_cli_opts_t* opts) {
    return _http_cli_exec("GET", url, NULL, 0, NULL, opts);
}

xylem_http_res_t* xylem_http_post(const char* url,
                                          const void* body, size_t body_len,
                                          const char* content_type,
                                          const xylem_http_cli_opts_t* opts) {
    return _http_cli_exec("POST", url, body, body_len, content_type, opts);
}

xylem_http_res_t* xylem_http_put(const char* url,
                                         const void* body, size_t body_len,
                                         const char* content_type,
                                         const xylem_http_cli_opts_t* opts) {
    return _http_cli_exec("PUT", url, body, body_len, content_type, opts);
}

xylem_http_res_t* xylem_http_delete(const char* url,
                                            const xylem_http_cli_opts_t* opts) {
    return _http_cli_exec("DELETE", url, NULL, 0, NULL, opts);
}

xylem_http_res_t* xylem_http_patch(const char* url,
                                           const void* body, size_t body_len,
                                           const char* content_type,
                                           const xylem_http_cli_opts_t* opts) {
    return _http_cli_exec("PATCH", url, body, body_len, content_type, opts);
}

static _http_session_pool_entry_t* _http_session_pool_entry_find_or_create(
    xylem_xrbtree_t* tree, const char* key) {
    _http_session_pool_entry_t* entry = xylem_xrbtree_find(tree, key);
    if (entry) {
        return entry;
    }
    entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return NULL;
    }
    snprintf(entry->key, sizeof(entry->key), "%s", key);
    xylem_xlist_init(&entry->idle_conns);
    if (xylem_xrbtree_insert(tree, entry) != 0) {
        free(entry);
        return NULL;
    }
    return entry;
}

static _http_session_idle_conn_t* _http_session_pool_get(
    xylem_http_session_t* session, const char* key) {
    _http_session_pool_entry_t* entry =
        xylem_xrbtree_find(&session->conn_pool, key);
    if (!entry) {
        return NULL;
    }
    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    while (!xylem_xlist_empty(&entry->idle_conns)) {
        _http_session_idle_conn_t* ic = xylem_xlist_head(&entry->idle_conns);
        xylem_xlist_remove(&entry->idle_conns, ic);
        if (ic->idle_since_ms + session->idle_timeout_ms < now) {
            ic->vt->close_conn(ic->transport);
            free(ic);
            continue;
        }
        return ic;
    }
    return NULL;
}

static void _http_session_pool_put(xylem_http_session_t* session,
                                   const char* key,
                                   const http_transport_vt_t* vt,
                                   void* transport) {
    _http_session_pool_entry_t* entry =
        _http_session_pool_entry_find_or_create(&session->conn_pool, key);
    if (!entry) {
        vt->close_conn(transport);
        return;
    }
    /* Evict oldest if pool is full. */
    while (xylem_xlist_len(&entry->idle_conns) >= session->max_idle_per_host) {
        _http_session_idle_conn_t* oldest =
            xylem_xlist_tail(&entry->idle_conns);
        if (!oldest) {
            break;
        }
        xylem_xlist_remove(&entry->idle_conns, oldest);
        oldest->vt->close_conn(oldest->transport);
        free(oldest);
    }
    _http_session_idle_conn_t* ic = calloc(1, sizeof(*ic));
    if (!ic) {
        vt->close_conn(transport);
        return;
    }
    ic->vt = vt;
    ic->transport = transport;
    ic->idle_since_ms = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    if (xylem_xlist_insert_head(&entry->idle_conns, ic) != 0) {
        vt->close_conn(transport);
        free(ic);
    }
}

static xylem_http_res_t* _http_session_exec(
    xylem_http_session_t* session,
    const char* method,
    const char* url,
    const void* body,
    size_t body_len,
    const char* content_type,
    const xylem_http_cli_opts_t* opts) {
    if (!session || !method || !url) {
        return NULL;
    }

    uint64_t timeout_ms    = (opts && opts->timeout_ms)    ? opts->timeout_ms    : DEFAULT_TIMEOUT_MS;
    int      max_redirects = (opts)                        ? opts->max_redirects  : 0;
    size_t   max_body_size = (opts && opts->max_body_size) ? opts->max_body_size  : DEFAULT_MAX_BODY;

    /* cookie_jar: per-request opts override session-level. */
    xylem_http_cookie_jar_t* cookie_jar = session->cookie_jar;
    if (opts && opts->cookie_jar) {
        cookie_jar = opts->cookie_jar;
    }

    _http_cli_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    if (http_url_parse(url, &ctx.url) != 0) {
        return NULL;
    }

    ctx.method              = method;
    ctx.body                = body;
    ctx.body_len            = body_len;
    ctx.content_type        = content_type;
    ctx.redirects_remaining = max_redirects;
    ctx.max_body_size       = max_body_size;
    ctx.custom_headers      = (opts) ? opts->headers      : NULL;
    ctx.custom_header_count = (opts) ? opts->header_count  : 0;
    ctx.cookie_jar          = cookie_jar;
    ctx.range               = (opts) ? opts->range         : NULL;
    ctx.is_session          = true;
    /* Borrow the session's persistent loop (by value copy for the ctx). */
    ctx.loop                = session->loop;

    for (;;) {
        ctx.done              = false;
        ctx.timed_out         = false;
        ctx.expect_continue   = false;
        ctx.continue_received = false;
        ctx.conn              = NULL;
        ctx.cur_header_name   = NULL;

        ctx.res = calloc(1, sizeof(*ctx.res));
        if (!ctx.res) {
            break;
        }

        llhttp_settings_init(&ctx.settings);
        ctx.settings.on_header_field     = _http_cli_res_header_field_cb;
        ctx.settings.on_header_value     = _http_cli_res_header_value_cb;
        ctx.settings.on_headers_complete = _http_cli_res_headers_complete_cb;
        ctx.settings.on_body             = _http_cli_res_body_cb;
        ctx.settings.on_message_complete = _http_cli_res_message_complete_cb;
        llhttp_init(&ctx.parser, HTTP_RESPONSE, &ctx.settings);
        ctx.parser.data = &ctx;

        ctx.timeout_timer = xylem_loop_create_timer(ctx.loop, &ctx);
        if (timeout_ms > 0) {
            xylem_loop_start_timer(ctx.timeout_timer,
                                   _http_cli_timeout_cb,
                                   timeout_ms, 0);
        }

        /* Try to reuse a pooled connection. */
        char pool_key[320];
        _http_session_make_key(&ctx.url, pool_key, sizeof(pool_key));
        _http_session_idle_conn_t* idle =
            _http_session_pool_get(session, pool_key);

        if (idle) {
            ctx.vt   = idle->vt;
            ctx.conn = idle->transport;
            free(idle);

            /* Re-wire callbacks for this request. */
            ctx.transport_cb.on_connect    = _http_cli_conn_connect_cb;
            ctx.transport_cb.on_read       = _http_cli_conn_read_cb;
            ctx.transport_cb.on_close      = _http_cli_conn_close_cb;
            ctx.transport_cb.on_write_done = NULL;
            ctx.transport_cb.on_accept     = NULL;
            ctx.vt->set_userdata(ctx.conn, &ctx);

            /* Directly send the request (skip DNS + dial). */
            _http_cli_conn_connect_cb(ctx.conn, &ctx);

            xylem_loop_run(ctx.loop);

            free(ctx.cur_header_name);
            ctx.cur_header_name = NULL;

            /* Stale connection: peer closed before we got a response. */
            if (!ctx.done && !ctx.timed_out && ctx.conn == NULL) {
                xylem_http_res_destroy(ctx.res);
                ctx.res = NULL;
                /* Transparent retry with a fresh connection. */
                ctx.res = calloc(1, sizeof(*ctx.res));
                if (!ctx.res) {
                    break;
                }
                llhttp_settings_init(&ctx.settings);
                ctx.settings.on_header_field     = _http_cli_res_header_field_cb;
                ctx.settings.on_header_value     = _http_cli_res_header_value_cb;
                ctx.settings.on_headers_complete = _http_cli_res_headers_complete_cb;
                ctx.settings.on_body             = _http_cli_res_body_cb;
                ctx.settings.on_message_complete = _http_cli_res_message_complete_cb;
                llhttp_init(&ctx.parser, HTTP_RESPONSE, &ctx.settings);
                ctx.parser.data = &ctx;
                ctx.done = false;
                ctx.timed_out = false;

                ctx.timeout_timer = xylem_loop_create_timer(ctx.loop, &ctx);
                if (timeout_ms > 0) {
                    xylem_loop_start_timer(ctx.timeout_timer,
                                           _http_cli_timeout_cb,
                                           timeout_ms, 0);
                }
                goto fresh_connect;
            }
        } else {
fresh_connect:

            xylem_addr_resolve_t* resolve_req =
                xylem_addr_resolve(ctx.loop, session->pool,
                                   ctx.url.host, ctx.url.port,
                                   _http_cli_resolve_cb, &ctx);
            if (!resolve_req) {
                _http_session_abort(&ctx);
                xylem_http_res_destroy(ctx.res);
                ctx.res = NULL;
                break;
            }

            xylem_loop_run(ctx.loop);

            free(ctx.cur_header_name);
            ctx.cur_header_name = NULL;
        }

        if (ctx.timed_out || !ctx.done) {
            if (ctx.conn) {
                ctx.vt->close_conn(ctx.conn);
                ctx.conn = NULL;
            }
            xylem_http_res_destroy(ctx.res);
            ctx.res = NULL;
            break;
        }

        /* Collect Set-Cookie headers into jar. */
        if (ctx.cookie_jar && ctx.res) {
            _http_cli_cookie_jar_collect(ctx.cookie_jar,
                                         ctx.res->headers,
                                         ctx.res->header_count,
                                         ctx.url.host, ctx.url.path);
        }

        free(ctx.merged_headers);
        ctx.merged_headers = NULL;
        ctx.merged_header_count = 0;

        /* Return connection to pool or close it. */
        if (ctx.conn) {
            const char* conn_hdr = http_header_find(
                ctx.res->headers, ctx.res->header_count, "Connection");
            bool keep_alive = true;
            if (conn_hdr && http_header_eq(conn_hdr, "close")) {
                keep_alive = false;
            }
            if (keep_alive) {
                /* Detach from loop IO before pooling. */
                _http_session_pool_put(session, pool_key,
                                       ctx.vt, ctx.conn);
                ctx.conn = NULL;
            } else {
                ctx.vt->close_conn(ctx.conn);
                ctx.conn = NULL;
            }
        }

        if (!_http_cli_follow_redirect(&ctx)) {
            break;
        }
    }

    /* Write back loop state to session. */
    session->loop = ctx.loop;
    free(ctx.merged_headers);
    return ctx.res;
}

xylem_http_session_t* xylem_http_session_create(
    const xylem_http_session_opts_t* opts) {
    xylem_http_session_t* s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->loop = xylem_loop_create();
    if (!s->loop) {
        free(s);
        return NULL;
    }
    s->pool = xylem_thrdpool_create(1);
    if (!s->pool) {
        xylem_loop_destroy(s->loop);
        free(s);
        return NULL;
    }
    xylem_xrbtree_init(&s->conn_pool,
                        _http_session_pool_cmp_dd,
                        _http_session_pool_cmp_kd);
    s->max_idle_per_host = (opts && opts->max_idle_per_host)
                               ? opts->max_idle_per_host
                               : DEFAULT_MAX_IDLE;
    s->idle_timeout_ms   = (opts && opts->idle_timeout_ms)
                               ? opts->idle_timeout_ms
                               : DEFAULT_IDLE_TIMEOUT_MS;
    s->cookie_jar        = (opts) ? opts->cookie_jar : NULL;
    return s;
}

void xylem_http_session_destroy(xylem_http_session_t* session) {
    if (!session) {
        return;
    }
    /* Close all pooled connections. */
    for (;;) {
        _http_session_pool_entry_t* entry =
            xylem_xrbtree_first(&session->conn_pool);
        if (!entry) {
            break;
        }
        while (!xylem_xlist_empty(&entry->idle_conns)) {
            _http_session_idle_conn_t* ic =
                xylem_xlist_head(&entry->idle_conns);
            xylem_xlist_remove(&entry->idle_conns, ic);
            ic->vt->close_conn(ic->transport);
            free(ic);
        }
        xylem_xrbtree_erase(&session->conn_pool, entry->key);
        free(entry);
    }
    xylem_thrdpool_destroy(session->pool);
    xylem_loop_destroy(session->loop);
    free(session);
}

xylem_http_res_t* xylem_http_session_get(
    xylem_http_session_t* session,
    const char* url,
    const xylem_http_cli_opts_t* opts) {
    return _http_session_exec(session, "GET", url, NULL, 0, NULL, opts);
}

xylem_http_res_t* xylem_http_session_post(
    xylem_http_session_t* session,
    const char* url,
    const void* body, size_t body_len,
    const char* content_type,
    const xylem_http_cli_opts_t* opts) {
    return _http_session_exec(session, "POST", url, body, body_len,
                              content_type, opts);
}

xylem_http_res_t* xylem_http_session_put(
    xylem_http_session_t* session,
    const char* url,
    const void* body, size_t body_len,
    const char* content_type,
    const xylem_http_cli_opts_t* opts) {
    return _http_session_exec(session, "PUT", url, body, body_len,
                              content_type, opts);
}

xylem_http_res_t* xylem_http_session_delete(
    xylem_http_session_t* session,
    const char* url,
    const xylem_http_cli_opts_t* opts) {
    return _http_session_exec(session, "DELETE", url, NULL, 0, NULL, opts);
}

xylem_http_res_t* xylem_http_session_patch(
    xylem_http_session_t* session,
    const char* url,
    const void* body, size_t body_len,
    const char* content_type,
    const xylem_http_cli_opts_t* opts) {
    return _http_session_exec(session, "PATCH", url, body, body_len,
                              content_type, opts);
}
