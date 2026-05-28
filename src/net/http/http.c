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

#include "http.h"
#include "xylem/encoding/xylem-gzip.h"

#include "encoding/gzip/miniz/miniz.h"
#include "net/http/llhttp/llhttp.h"
#include "runtime/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int _transport_read(http_transport_t* t, void* buf, int len) {
    return t->read(t->conn, buf, len);
}

static inline int _transport_write(http_transport_t* t, const void* data, int len) {
    return t->write(t->conn, data, len);
}

static inline void _transport_close(http_transport_t* t) {
    t->close(t->conn);
}

#define POOL_MAX_IDLE_PER_HOST  5
#define POOL_IDLE_TIMEOUT_MS    90000
#define POOL_MAX_HOSTS          32

typedef struct {
    http_transport_t transport;
    uint64_t         idle_since; /* monotonic ms timestamp */
} _pool_idle_conn_t;

typedef struct {
    char               key[320]; /* "host:port:scheme" */
    _pool_idle_conn_t* conns;    /* dynamic array of idle conns */
    size_t             count;
    size_t             cap;
} _pool_entry_t;

static _pool_entry_t _pool[POOL_MAX_HOSTS];
static size_t        _pool_size;

/* Find or create a pool entry for the given key. Returns NULL if full. */
static _pool_entry_t* _pool_find_entry(const char* key) {
    for (size_t i = 0; i < _pool_size; i++) {
        if (strcmp(_pool[i].key, key) == 0) {
            return &_pool[i];
        }
    }
    return NULL;
}

/**
 * Try to acquire an idle connection for the given parsed URL.
 * Evicts stale connections. Returns true and fills *out on success.
 */
static bool _pool_acquire(const http_url_t* url, http_transport_t* out) {
    char key[320];
    snprintf(key, sizeof(key), "%s:%u:%s", url->host, (unsigned)url->port,
             url->scheme);

    _pool_entry_t* entry = _pool_find_entry(key);
    if (!entry || entry->count == 0) {
        return false;
    }

    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    /* Scan from back (LIFO) -- most recently returned connections first. */
    while (entry->count > 0) {
        size_t idx = entry->count - 1;
        _pool_idle_conn_t* ic = &entry->conns[idx];

        if (now - ic->idle_since > POOL_IDLE_TIMEOUT_MS) {
            /* Stale -- close and remove. */
            _transport_close(&ic->transport);
            entry->count--;
            continue;
        }

        /* Valid idle connection found. */
        *out = ic->transport;
        entry->count--;
        return true;
    }

    return false;
}

/**
 * Return a keep-alive connection to the pool for later reuse.
 * If the pool for this host is full, the oldest idle connection is closed.
 */
static void _pool_release(const http_url_t* url, http_transport_t* t) {
    char key[320];
    snprintf(key, sizeof(key), "%s:%u:%s", url->host, (unsigned)url->port,
             url->scheme);

    _pool_entry_t* entry = _pool_find_entry(key);
    if (!entry) {
        /* Create new entry. */
        if (_pool_size >= POOL_MAX_HOSTS) {
            /* Pool full -- just close the connection. */
            _transport_close(t);
            return;
        }
        entry = &_pool[_pool_size++];
        snprintf(entry->key, sizeof(entry->key), "%s", key);
        entry->conns = NULL;
        entry->count = 0;
        entry->cap = 0;
    }

    /* Evict oldest if at capacity. */
    if (entry->count >= POOL_MAX_IDLE_PER_HOST) {
        _transport_close(&entry->conns[0].transport);
        memmove(&entry->conns[0], &entry->conns[1],
                (entry->count - 1) * sizeof(_pool_idle_conn_t));
        entry->count--;
    }

    /* Grow array if needed. */
    if (entry->count >= entry->cap) {
        size_t new_cap = entry->cap == 0 ? 4 : entry->cap * 2;
        if (new_cap > POOL_MAX_IDLE_PER_HOST) {
            new_cap = POOL_MAX_IDLE_PER_HOST;
        }
        _pool_idle_conn_t* tmp = (_pool_idle_conn_t*)realloc(
            entry->conns, new_cap * sizeof(_pool_idle_conn_t));
        if (!tmp) {
            _transport_close(t);
            return;
        }
        entry->conns = tmp;
        entry->cap = new_cap;
    }

    entry->conns[entry->count].transport = *t;
    entry->conns[entry->count].idle_since =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    entry->count++;
}

struct xylem_http_req_s {
    char           method[16];
    char*          url;
    size_t         url_len;
    http_header_t* headers;
    size_t         header_count;
    size_t         header_cap;
    uint8_t*       body;
    size_t         body_len;
    size_t         body_cap;
    http_header_t* params;       /* path parameters (name/value pairs) */
    size_t         param_count;
    size_t         param_cap;
};

struct xylem_http_res_s {
    /* Shared read fields (client result & server post-flush read-back) */
    int            status_code;
    http_header_t* headers;
    size_t         header_count;
    size_t         header_cap;
    uint8_t*       body;
    size_t         body_len;
    size_t         body_cap;

    /* Server writer state (only meaningful during handler) */
    http_transport_t* _transport;  /* Non-NULL only while handler runs */
    bool              _headers_sent;

    /* Server gzip compression state */
    mz_stream*     gzip_stream;
    bool           gzip_active;
    bool           accept_gzip;          /* client sent Accept-Encoding: gzip */
    const xylem_http_gzip_opts_t* _gzip_opts; /* points to srv->gzip_opts */
};

const char* xylem_http_req_method(const xylem_http_req_t* req) {
    return req ? req->method : NULL;
}

const char* xylem_http_req_url(const xylem_http_req_t* req) {
    return req ? req->url : NULL;
}

const char* xylem_http_req_header(const xylem_http_req_t* req,
                                  const char* name) {
    if (!req || !name) {
        return NULL;
    }
    return http_header_find(req->headers, req->header_count, name);
}

const void* xylem_http_req_body(const xylem_http_req_t* req) {
    return req ? (const void*)req->body : NULL;
}

size_t xylem_http_req_body_len(const xylem_http_req_t* req) {
    return req ? req->body_len : 0;
}

const char* xylem_http_req_param(const xylem_http_req_t* req,
                                 const char* name) {
    if (!req || !name) {
        return NULL;
    }
    for (size_t i = 0; i < req->param_count; i++) {
        if (strcmp(req->params[i].name, name) == 0) {
            return req->params[i].value;
        }
    }
    return NULL;
}

int xylem_http_res_status(const xylem_http_res_t* res) {
    return res ? res->status_code : 0;
}

const char* xylem_http_res_header(const xylem_http_res_t* res,
                                  const char* name) {
    if (!res || !name) {
        return NULL;
    }
    return http_header_find(res->headers, res->header_count, name);
}

const void* xylem_http_res_body(const xylem_http_res_t* res) {
    return res ? (const void*)res->body : NULL;
}

size_t xylem_http_res_body_len(const xylem_http_res_t* res) {
    return res ? res->body_len : 0;
}

void xylem_http_res_destroy(xylem_http_res_t* res) {
    if (!res) {
        return;
    }
    http_headers_free(res->headers, res->header_count);
    free(res->body);
    free(res);
}

int xylem_http_res_set_status(xylem_http_res_t* res, int code) {
    if (!res || res->_headers_sent) {
        return -1;
    }
    res->status_code = code;
    return 0;
}

int xylem_http_res_set_header(xylem_http_res_t* res,
                              const char* name, const char* value) {
    if (!res || !name || !value || res->_headers_sent) {
        return -1;
    }
    return http_header_add(&res->headers, &res->header_count,
                           &res->header_cap, name, strlen(name),
                           value, strlen(value));
}

static const char* _default_compressible_types[] = {
    "text/html",
    "text/css",
    "text/plain",
    "text/xml",
    "text/javascript",
    "application/json",
    "application/javascript",
    "application/xml",
    "image/svg+xml",
};

static const size_t _default_compressible_count =
    sizeof(_default_compressible_types) / sizeof(_default_compressible_types[0]);

/**
 * Check if a Content-Type is compressible.
 * Compares the type prefix (before any ;params).
 */
static bool _is_compressible(const char* content_type,
                             const xylem_http_gzip_opts_t* opts) {
    if (!content_type) {
        return false;
    }

    /* Extract the base type (before ';' if present). */
    size_t ct_len = strlen(content_type);
    const char* semi = strchr(content_type, ';');
    if (semi) {
        ct_len = (size_t)(semi - content_type);
    }
    /* Trim trailing spaces. */
    while (ct_len > 0 && content_type[ct_len - 1] == ' ') {
        ct_len--;
    }

    /* If user provided custom mime_types, check against those. */
    if (opts->mime_types && opts->mime_types[0]) {
        /* Simple comma-separated check. */
        const char* p = opts->mime_types;
        while (*p) {
            /* Skip leading whitespace. */
            while (*p == ' ' || *p == ',') p++;
            if (!*p) break;
            const char* end = p;
            while (*end && *end != ',') end++;
            size_t tlen = (size_t)(end - p);
            /* Trim trailing spaces. */
            while (tlen > 0 && p[tlen - 1] == ' ') tlen--;
            if (tlen == ct_len && strncmp(p, content_type, ct_len) == 0) {
                return true;
            }
            p = end;
        }
        return false;
    }

    /* Check against default list. */
    for (size_t i = 0; i < _default_compressible_count; i++) {
        size_t dlen = strlen(_default_compressible_types[i]);
        if (dlen == ct_len &&
            strncmp(_default_compressible_types[i], content_type, ct_len) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Decide whether to activate gzip compression for this response.
 * Must be called before _flush_headers.
 */
static void _maybe_init_gzip(xylem_http_res_t* res) {
    if (!res->accept_gzip || !res->_gzip_opts ||
        !res->_gzip_opts->enabled) {
        return;
    }

    /* Check if Content-Type is compressible. */
    const char* ct = http_header_find(res->headers, res->header_count,
                                      "Content-Type");
    if (!_is_compressible(ct, res->_gzip_opts)) {
        return;
    }

    /* For streaming (chunked), always compress if enabled.
     * min_size check only if Content-Length is known (we don't have it here). */

    /* Init deflate stream. */
    mz_stream* s = (mz_stream*)calloc(1, sizeof(mz_stream));
    if (!s) {
        return;
    }

    int level = res->_gzip_opts->level;
    if (level == 0) {
        level = MZ_DEFAULT_COMPRESSION;
    }

    int rc = mz_deflateInit2(s, level, MZ_DEFLATED,
                             15 + 16, /* gzip format */
                             8, MZ_DEFAULT_STRATEGY);
    if (rc != MZ_OK) {
        free(s);
        return;
    }

    res->gzip_stream = s;
    res->gzip_active = true;

    /* Add Content-Encoding header. */
    http_header_add(&res->headers, &res->header_count, &res->header_cap,
                    "Content-Encoding", 16, "gzip", 4);
}

/**
 * Flush status line + headers with Transfer-Encoding: chunked.
 * Called on the first xylem_http_res_write().
 */
static int _flush_headers(xylem_http_res_t* res) {
    if (!res->_transport) {
        return -1;
    }

    /* Decide on gzip before writing headers. */
    _maybe_init_gzip(res);

    int status = res->status_code ? res->status_code : 200;
    const char* reason = http_reason_phrase(status);

    /* Build the response head into a buffer. */
    /* "HTTP/1.1 XXX Reason\r\n" + headers + "Transfer-Encoding: chunked\r\n\r\n" */
    size_t est = 64 + strlen(reason);
    for (size_t i = 0; i < res->header_count; i++) {
        est += strlen(res->headers[i].name) + strlen(res->headers[i].value) + 4;
    }
    est += 32; /* Transfer-Encoding: chunked\r\n\r\n */

    char* buf = (char*)malloc(est);
    if (!buf) {
        return -1;
    }

    int off = snprintf(buf, est, "HTTP/1.1 %d %s\r\n", status, reason);

    for (size_t i = 0; i < res->header_count; i++) {
        off += snprintf(buf + off, est - (size_t)off, "%s: %s\r\n",
                        res->headers[i].name, res->headers[i].value);
    }
    off += snprintf(buf + off, est - (size_t)off,
                    "Transfer-Encoding: chunked\r\n\r\n");

    int rc = _transport_write(res->_transport, buf, off);
    free(buf);
    if (rc != 0) {
        return -1;
    }
    res->_headers_sent = true;
    return 0;
}

/**
 * Write a single chunked-encoded chunk to the transport.
 */
static int _write_chunk(xylem_http_res_t* res, const void* data, size_t len) {
    char chunk_hdr[24];
    int hdr_len = snprintf(chunk_hdr, sizeof(chunk_hdr), "%zx\r\n", len);
    if (_transport_write(res->_transport, chunk_hdr, hdr_len) != 0) {
        return -1;
    }
    if (_transport_write(res->_transport, data, (int)len) != 0) {
        return -1;
    }
    if (_transport_write(res->_transport, "\r\n", 2) != 0) {
        return -1;
    }
    return 0;
}

int xylem_http_res_write(xylem_http_res_t* res,
                         const void* data, size_t len) {
    if (!res || !res->_transport) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    /* Flush headers on first write. */
    if (!res->_headers_sent) {
        if (_flush_headers(res) != 0) {
            return -1;
        }
    }

    if (res->gzip_active && res->gzip_stream) {
        /* Compress through gzip stream. */
        mz_stream* s = res->gzip_stream;
        s->next_in = (const unsigned char*)data;
        s->avail_in = (mz_uint32)len;

        uint8_t out_buf[4096];
        while (s->avail_in > 0) {
            s->next_out = out_buf;
            s->avail_out = (mz_uint32)sizeof(out_buf);
            int rc = mz_deflate(s, MZ_NO_FLUSH);
            if (rc != MZ_OK && rc != MZ_BUF_ERROR) {
                return -1;
            }
            size_t produced = sizeof(out_buf) - s->avail_out;
            if (produced > 0) {
                if (_write_chunk(res, out_buf, produced) != 0) {
                    return -1;
                }
            }
        }
        return 0;
    }

    /* Send chunk: "<hex-len>\r\n<data>\r\n" */
    return _write_chunk(res, data, len);
}

/**
 * Send the terminating chunk "0\r\n\r\n" after the handler returns.
 * If no writes happened, flush empty response first.
 * If gzip is active, flush the gzip stream with MZ_FINISH first.
 */
static void _finalize_response(xylem_http_res_t* res) {
    if (!res->_transport) {
        return;
    }
    if (!res->_headers_sent) {
        /* Handler returned without writing -- send headers + empty body. */
        _flush_headers(res);
    }

    /* Flush gzip stream if active. */
    if (res->gzip_active && res->gzip_stream) {
        mz_stream* s = res->gzip_stream;
        uint8_t out_buf[4096];
        s->next_in = NULL;
        s->avail_in = 0;
        int rc;
        do {
            s->next_out = out_buf;
            s->avail_out = (mz_uint32)sizeof(out_buf);
            rc = mz_deflate(s, MZ_FINISH);
            size_t produced = sizeof(out_buf) - s->avail_out;
            if (produced > 0) {
                _write_chunk(res, out_buf, produced);
            }
        } while (rc == MZ_OK);

        mz_deflateEnd(s);
        free(s);
        res->gzip_stream = NULL;
        res->gzip_active = false;
    }

    /* Send terminating chunk. */
    _transport_write(res->_transport, "0\r\n\r\n", 5);
}

typedef struct {
    llhttp_t          parser;
    llhttp_settings_t settings;
    xylem_http_req_t  req;
    char*             cur_hdr_name;
    size_t            cur_hdr_name_len;
    bool              complete;
} _srv_parser_t;

static int _srv_on_method(llhttp_t* p, const char* at, size_t len) {
    _srv_parser_t* ctx = (_srv_parser_t*)p->data;
    size_t copy = len < sizeof(ctx->req.method) - 1
                  ? len : sizeof(ctx->req.method) - 1;
    memcpy(ctx->req.method, at, copy);
    ctx->req.method[copy] = '\0';
    return 0;
}

static int _srv_on_url(llhttp_t* p, const char* at, size_t len) {
    _srv_parser_t* ctx = (_srv_parser_t*)p->data;
    char* new_url = (char*)realloc(ctx->req.url, ctx->req.url_len + len + 1);
    if (!new_url) {
        return -1;
    }
    memcpy(new_url + ctx->req.url_len, at, len);
    ctx->req.url_len += len;
    new_url[ctx->req.url_len] = '\0';
    ctx->req.url = new_url;
    return 0;
}

static int _srv_on_header_field(llhttp_t* p, const char* at, size_t len) {
    _srv_parser_t* ctx = (_srv_parser_t*)p->data;
    char* n = (char*)realloc(ctx->cur_hdr_name,
                             ctx->cur_hdr_name_len + len + 1);
    if (!n) {
        return -1;
    }
    memcpy(n + ctx->cur_hdr_name_len, at, len);
    ctx->cur_hdr_name_len += len;
    n[ctx->cur_hdr_name_len] = '\0';
    ctx->cur_hdr_name = n;
    return 0;
}

static int _srv_on_header_value(llhttp_t* p, const char* at, size_t len) {
    _srv_parser_t* ctx = (_srv_parser_t*)p->data;
    /* Append header. */
    http_header_add(&ctx->req.headers, &ctx->req.header_count,
                    &ctx->req.header_cap,
                    ctx->cur_hdr_name, ctx->cur_hdr_name_len,
                    at, len);
    return 0;
}

static int _srv_on_header_value_complete(llhttp_t* p) {
    _srv_parser_t* ctx = (_srv_parser_t*)p->data;
    free(ctx->cur_hdr_name);
    ctx->cur_hdr_name = NULL;
    ctx->cur_hdr_name_len = 0;
    return 0;
}

static int _srv_on_body(llhttp_t* p, const char* at, size_t len) {
    _srv_parser_t* ctx = (_srv_parser_t*)p->data;
    if (ctx->req.body_len + len > ctx->req.body_cap) {
        size_t new_cap = ctx->req.body_cap ? ctx->req.body_cap * 2 : 4096;
        while (new_cap < ctx->req.body_len + len) {
            new_cap *= 2;
        }
        uint8_t* nb = (uint8_t*)realloc(ctx->req.body, new_cap);
        if (!nb) {
            return -1;
        }
        ctx->req.body = nb;
        ctx->req.body_cap = new_cap;
    }
    memcpy(ctx->req.body + ctx->req.body_len, at, len);
    ctx->req.body_len += len;
    return 0;
}

static int _srv_on_message_complete(llhttp_t* p) {
    _srv_parser_t* ctx = (_srv_parser_t*)p->data;
    ctx->complete = true;
    return HPE_PAUSED;
}

static void _srv_parser_init(_srv_parser_t* sp) {
    memset(sp, 0, sizeof(*sp));
    llhttp_settings_init(&sp->settings);
    sp->settings.on_method              = _srv_on_method;
    sp->settings.on_url                 = _srv_on_url;
    sp->settings.on_header_field        = _srv_on_header_field;
    sp->settings.on_header_value        = _srv_on_header_value;
    sp->settings.on_header_value_complete = _srv_on_header_value_complete;
    sp->settings.on_body                = _srv_on_body;
    sp->settings.on_message_complete    = _srv_on_message_complete;
    llhttp_init(&sp->parser, HTTP_REQUEST, &sp->settings);
    sp->parser.data = sp;
}

static void _srv_parser_reset(_srv_parser_t* sp) {
    free(sp->req.url);
    http_headers_free(sp->req.headers, sp->req.header_count);
    http_headers_free(sp->req.params, sp->req.param_count);
    free(sp->req.body);
    free(sp->cur_hdr_name);
    memset(&sp->req, 0, sizeof(sp->req));
    sp->cur_hdr_name = NULL;
    sp->cur_hdr_name_len = 0;
    sp->complete = false;
    llhttp_reset(&sp->parser);
}

static void _srv_parser_destroy(_srv_parser_t* sp) {
    free(sp->req.url);
    http_headers_free(sp->req.headers, sp->req.header_count);
    http_headers_free(sp->req.params, sp->req.param_count);
    free(sp->req.body);
    free(sp->cur_hdr_name);
}

void http_srv_conn_coroutine(void* arg) {
    http_srv_conn_ctx_t* ctx = (http_srv_conn_ctx_t*)arg;
    _srv_parser_t sp;
    _srv_parser_init(&sp);

    char readbuf[4096];
    bool keep_alive = true;

    while (keep_alive) {
        sp.complete = false;

        /* Read loop until we have a complete request. */
        while (!sp.complete) {
            int n = _transport_read(&ctx->transport, readbuf, (int)sizeof(readbuf));
            if (n <= 0) {
                goto done; /* peer closed or error */
            }

            llhttp_errno_t err = llhttp_execute(&sp.parser, readbuf, (size_t)n);
            if (err == HPE_PAUSED) {
                /* message_complete fired -- request is ready. */
                llhttp_resume(&sp.parser);
            } else if (err != HPE_OK) {
                /* Parse error -- send 400 and close. */
                const char* bad =
                    "HTTP/1.1 400 Bad Request\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n";
                _transport_write(&ctx->transport, bad, (int)strlen(bad));
                goto done;
            }
        }

        /* Determine keep-alive. */
        keep_alive = llhttp_should_keep_alive(&sp.parser) != 0;

        /* Build response object for the handler. */
        xylem_http_res_t res;
        memset(&res, 0, sizeof(res));
        res._transport = &ctx->transport;
        res.status_code = 200;

        /* Check if client accepts gzip encoding. */
        const char* ae = http_header_find(sp.req.headers, sp.req.header_count,
                                          "Accept-Encoding");
        if (ae && strstr(ae, "gzip")) {
            res.accept_gzip = true;
        }
        if (ctx->srv->gzip_opts.enabled) {
            res._gzip_opts = &ctx->srv->gzip_opts;
        }

        /* Call the user handler. */
        ctx->srv->handler(&res, &sp.req, ctx->srv->userdata);

        /* Auto-finalize the response. */
        _finalize_response(&res);

        /* Clean up response headers (allocated by handler). */
        http_headers_free(res.headers, res.header_count);

        /* Reset parser for next request on same connection. */
        _srv_parser_reset(&sp);
    }

done:
    _srv_parser_destroy(&sp);
    _transport_close(&ctx->transport);
    free(ctx);
}

void xylem_http_srv_set_gzip(xylem_http_srv_t* srv,
                                  const xylem_http_gzip_opts_t* opts) {
    if (!srv || !opts) {
        return;
    }
    ((http_srv_t*)srv)->gzip_opts = *opts;
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

xylem_http_cookie_jar_t* xylem_http_cookie_jar_create(void) {
    xylem_http_cookie_jar_t* jar =
        (xylem_http_cookie_jar_t*)calloc(1, sizeof(*jar));
    return jar;
}

static void _cookie_free(_cookie_t* c) {
    free(c->name);
    free(c->value);
    free(c->domain);
    free(c->path);
}

void xylem_http_cookie_jar_destroy(xylem_http_cookie_jar_t* jar) {
    if (!jar) {
        return;
    }
    for (size_t i = 0; i < jar->count; i++) {
        _cookie_free(&jar->cookies[i]);
    }
    free(jar->cookies);
    free(jar);
}

static char* _cookie_strdup(const char* s, size_t len) {
    char* d = (char*)malloc(len + 1);
    if (!d) {
        return NULL;
    }
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

static const char* _cookie_skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static bool _cookie_iprefix(const char* s, const char* prefix, size_t plen) {
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
static int _cookie_parse(const char* header, const char* req_host,
                         const char* req_path, _cookie_t* out) {
    memset(out, 0, sizeof(*out));

    /* name=value is before the first ';' */
    const char* semi = strchr(header, ';');
    size_t nv_len = semi ? (size_t)(semi - header) : strlen(header);

    const char* eq = (const char*)memchr(header, '=', nv_len);
    if (!eq || eq == header) {
        return -1;
    }

    size_t name_len = (size_t)(eq - header);
    size_t val_len  = nv_len - name_len - 1;

    out->name  = _cookie_strdup(header, name_len);
    out->value = _cookie_strdup(eq + 1, val_len);
    if (!out->name || !out->value) {
        _cookie_free(out);
        return -1;
    }

    /* Default domain and path from request. */
    out->domain = _cookie_strdup(req_host, strlen(req_host));
    out->path   = _cookie_strdup("/", 1);
    if (!out->domain || !out->path) {
        _cookie_free(out);
        return -1;
    }

    /* Parse attributes. */
    const char* p = semi ? semi + 1 : NULL;
    while (p && *p) {
        p = _cookie_skip_ws(p);
        const char* next = strchr(p, ';');
        size_t attr_len = next ? (size_t)(next - p) : strlen(p);

        const char* aeq = (const char*)memchr(p, '=', attr_len);
        size_t key_len = aeq ? (size_t)(aeq - p) : attr_len;

        /* Trim trailing whitespace from key. */
        while (key_len > 0 && (p[key_len - 1] == ' ' || p[key_len - 1] == '\t')) {
            key_len--;
        }

        if (key_len == 6 && aeq && _cookie_iprefix(p, "domain", 6)) {
            const char* v = _cookie_skip_ws(aeq + 1);
            size_t vlen = attr_len - (size_t)(v - p);
            /* Strip leading dot. */
            if (vlen > 0 && v[0] == '.') {
                v++;
                vlen--;
            }
            free(out->domain);
            out->domain = _cookie_strdup(v, vlen);
        } else if (key_len == 4 && aeq && _cookie_iprefix(p, "path", 4)) {
            const char* v = _cookie_skip_ws(aeq + 1);
            size_t vlen = attr_len - (size_t)(v - p);
            free(out->path);
            out->path = _cookie_strdup(v, vlen);
        } else if (key_len == 7 && aeq && _cookie_iprefix(p, "max-age", 7)) {
            const char* v = _cookie_skip_ws(aeq + 1);
            long age = strtol(v, NULL, 10);
            if (age <= 0) {
                out->expires = 1; /* expired */
            } else {
                out->expires = xylem_utils_getnow(XYLEM_TIME_PRECISION_SEC)
                             + (uint64_t)age;
            }
        } else if (key_len == 6 && _cookie_iprefix(p, "secure", 6)) {
            out->secure = true;
        } else if (key_len == 8 && _cookie_iprefix(p, "httponly", 8)) {
            out->http_only = true;
        }

        p = next ? next + 1 : NULL;
    }

    (void)req_path;
    return 0;
}

/* Domain tail match: cookie domain "example.com" matches "sub.example.com". */
static bool _cookie_domain_match(const char* cookie_domain,
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
static bool _cookie_path_match(const char* cookie_path,
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
    if (cookie_path[cp_len - 1] == '/') {
        return true;
    }
    return req_path[cp_len] == '/';
}

static bool _cookie_match(const _cookie_t* c, const char* scheme,
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

    if (!_cookie_domain_match(c->domain, host)) {
        return false;
    }

    if (!_cookie_path_match(c->path, path)) {
        return false;
    }

    return true;
}

/* Store a cookie in the jar, replacing any existing same name+domain+path. */
static void _cookie_jar_store(xylem_http_cookie_jar_t* jar, _cookie_t* c) {
    for (size_t i = 0; i < jar->count; i++) {
        _cookie_t* existing = &jar->cookies[i];
        if (strcmp(existing->name, c->name) == 0 &&
            http_header_eq(existing->domain, c->domain) &&
            strcmp(existing->path, c->path) == 0) {
            _cookie_free(existing);
            *existing = *c;
            return;
        }
    }

    /* Grow array if needed. */
    if (jar->count >= jar->cap) {
        size_t new_cap = jar->cap ? jar->cap * 2 : 8;
        _cookie_t* tmp = (_cookie_t*)realloc(jar->cookies,
                                             new_cap * sizeof(*tmp));
        if (!tmp) {
            _cookie_free(c);
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
static void _cookie_jar_collect(xylem_http_cookie_jar_t* jar,
                                const http_header_t* headers,
                                size_t header_count,
                                const char* req_host,
                                const char* req_path) {
    for (size_t i = 0; i < header_count; i++) {
        if (strlen(headers[i].name) != 10 ||
            !http_header_eq(headers[i].name, "Set-Cookie")) {
            continue;
        }
        _cookie_t c;
        if (_cookie_parse(headers[i].value, req_host, req_path, &c) == 0) {
            _cookie_jar_store(jar, &c);
        }
    }
}

/**
 * Build a "name=val; name2=val2" cookie header value from matching cookies.
 * Returns malloc'd string or NULL if no cookies match.
 */
static char* _cookie_jar_build(const xylem_http_cookie_jar_t* jar,
                               const char* scheme, const char* host,
                               const char* path) {
    size_t cap = 128;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        return NULL;
    }

    size_t off = 0;
    bool first = true;
    for (size_t i = 0; i < jar->count; i++) {
        if (!_cookie_match(&jar->cookies[i], scheme, host, path)) {
            continue;
        }

        size_t nlen = strlen(jar->cookies[i].name);
        size_t vlen = strlen(jar->cookies[i].value);
        size_t need = nlen + 1 + vlen + (first ? 0 : 2);

        if (off + need + 1 > cap) {
            while (off + need + 1 > cap) {
                cap *= 2;
            }
            char* tmp = (char*)realloc(buf, cap);
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

typedef struct {
    llhttp_t          parser;
    llhttp_settings_t settings;
    xylem_http_res_t* res;
    char*             cur_hdr_name;
    size_t            cur_hdr_name_len;
    bool              complete;
} _cli_parser_t;

static int _cli_on_header_field(llhttp_t* p, const char* at, size_t len) {
    _cli_parser_t* ctx = (_cli_parser_t*)p->data;
    char* n = (char*)realloc(ctx->cur_hdr_name,
                             ctx->cur_hdr_name_len + len + 1);
    if (!n) {
        return -1;
    }
    memcpy(n + ctx->cur_hdr_name_len, at, len);
    ctx->cur_hdr_name_len += len;
    n[ctx->cur_hdr_name_len] = '\0';
    ctx->cur_hdr_name = n;
    return 0;
}

static int _cli_on_header_value(llhttp_t* p, const char* at, size_t len) {
    _cli_parser_t* ctx = (_cli_parser_t*)p->data;
    http_header_add(&ctx->res->headers, &ctx->res->header_count,
                    &ctx->res->header_cap,
                    ctx->cur_hdr_name, ctx->cur_hdr_name_len,
                    at, len);
    return 0;
}

static int _cli_on_header_value_complete(llhttp_t* p) {
    _cli_parser_t* ctx = (_cli_parser_t*)p->data;
    free(ctx->cur_hdr_name);
    ctx->cur_hdr_name = NULL;
    ctx->cur_hdr_name_len = 0;
    return 0;
}

static int _cli_on_headers_complete(llhttp_t* p) {
    _cli_parser_t* ctx = (_cli_parser_t*)p->data;
    ctx->res->status_code = (int)p->status_code;
    return 0;
}

static int _cli_on_body(llhttp_t* p, const char* at, size_t len) {
    _cli_parser_t* ctx = (_cli_parser_t*)p->data;
    xylem_http_res_t* res = ctx->res;
    if (res->body_len + len > res->body_cap) {
        size_t new_cap = res->body_cap ? res->body_cap * 2 : 4096;
        while (new_cap < res->body_len + len) {
            new_cap *= 2;
        }
        uint8_t* nb = (uint8_t*)realloc(res->body, new_cap);
        if (!nb) {
            return -1;
        }
        res->body = nb;
        res->body_cap = new_cap;
    }
    memcpy(res->body + res->body_len, at, len);
    res->body_len += len;
    return 0;
}

static int _cli_on_message_complete(llhttp_t* p) {
    _cli_parser_t* ctx = (_cli_parser_t*)p->data;
    ctx->complete = true;
    return HPE_PAUSED;
}

static void _cli_parser_init(_cli_parser_t* cp, xylem_http_res_t* res) {
    memset(cp, 0, sizeof(*cp));
    cp->res = res;
    llhttp_settings_init(&cp->settings);
    cp->settings.on_header_field          = _cli_on_header_field;
    cp->settings.on_header_value          = _cli_on_header_value;
    cp->settings.on_header_value_complete = _cli_on_header_value_complete;
    cp->settings.on_headers_complete      = _cli_on_headers_complete;
    cp->settings.on_body                  = _cli_on_body;
    cp->settings.on_message_complete      = _cli_on_message_complete;
    llhttp_init(&cp->parser, HTTP_RESPONSE, &cp->settings);
    cp->parser.data = cp;
}

static void _cli_parser_destroy(_cli_parser_t* cp) {
    free(cp->cur_hdr_name);
}

/**
 * Resolve a Location header value against the request URL.
 * Handles absolute URLs and relative paths (e.g., "/new-path").
 * Returns 0 on success with resolved URL in *out, -1 on failure.
 */
static int _resolve_redirect_url(const char* location, const http_url_t* base,
                                 http_url_t* out) {
    if (!location || !*location) {
        return -1;
    }

    /* If location is an absolute URL, parse it directly. */
    if (strstr(location, "://") != NULL) {
        return http_url_parse(location, out);
    }

    /* Relative URL -- resolve against base. */
    *out = *base;

    if (location[0] == '/') {
        /* Absolute path -- replace path entirely. */
        size_t loc_len = strlen(location);
        if (loc_len >= sizeof(out->path)) {
            return -1;
        }
        memcpy(out->path, location, loc_len + 1);
    } else {
        /* Relative path -- append to directory of base path. */
        /* Find last '/' in base path. */
        const char* last_slash = strrchr(base->path, '/');
        size_t dir_len = last_slash ? (size_t)(last_slash - base->path + 1) : 1;
        size_t loc_len = strlen(location);
        if (dir_len + loc_len >= sizeof(out->path)) {
            return -1;
        }
        memcpy(out->path, base->path, dir_len);
        memcpy(out->path + dir_len, location, loc_len + 1);
    }

    return 0;
}

xylem_http_res_t* http_do_request(
    const char*              method,
    const char*              url,
    const void*              body,
    size_t                   body_len,
    const char*              content_type,
    const xylem_http_opts_t* opts,
    http_dial_fn_t           dial_fn,
    void*                    dial_ctx) {

    if (!url || !dial_fn) {
        return NULL;
    }

    int redirects_left = 0;
    if (opts && opts->max_redirects > 0) {
        redirects_left = opts->max_redirects;
    }

    /* Working copies of method/body that may change on redirect. */
    const char* cur_method = method;
    const void* cur_body = body;
    size_t cur_body_len = body_len;
    const char* cur_content_type = content_type;

    /* Parse URL (working copy for redirect loop). */
    http_url_t parsed;
    if (http_url_parse(url, &parsed) != 0) {
        return NULL;
    }

redirect_loop:
    ;  /* empty statement for label before declaration */

    /* Try to reuse a pooled connection. */
    http_transport_t transport;
    memset(&transport, 0, sizeof(transport));
    bool from_pool = _pool_acquire(&parsed, &transport);

    if (!from_pool) {
        /* No pooled connection available -- dial a new one. */
        uint64_t timeout = 10000;
        if (opts && opts->timeout_ms > 0) {
            timeout = opts->timeout_ms;
        }
        transport = dial_fn(parsed.host, parsed.port, timeout, dial_ctx);
        if (!transport.conn) {
            return NULL;
        }
    }

    /* Set deadlines. */
    uint64_t deadline_ms = 0;
    if (opts && opts->timeout_ms > 0) {
        deadline_ms = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                      + opts->timeout_ms;
    } else {
        deadline_ms = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 30000;
    }
    if (transport.set_rd_deadline) {
        transport.set_rd_deadline(transport.conn, deadline_ms);
    }
    if (transport.set_wr_deadline) {
        transport.set_wr_deadline(transport.conn, deadline_ms);
    }

    /* Build custom headers, injecting Cookie header if jar is set. */
    xylem_http_hdr_t* merged_hdrs = NULL;
    size_t merged_hdr_count = 0;
    char* cookie_val = NULL;

    const xylem_http_hdr_t* custom_hdrs = NULL;
    size_t custom_hdr_count = 0;
    if (opts) {
        custom_hdrs = opts->headers;
        custom_hdr_count = opts->header_count;
    }

    if (opts && opts->cookie_jar) {
        cookie_val = _cookie_jar_build(opts->cookie_jar, parsed.scheme,
                                       parsed.host, parsed.path);
    }

    if (cookie_val) {
        /* Merge user headers + Cookie header. */
        merged_hdr_count = custom_hdr_count + 1;
        merged_hdrs = (xylem_http_hdr_t*)malloc(
            merged_hdr_count * sizeof(xylem_http_hdr_t));
        if (!merged_hdrs) {
            free(cookie_val);
            _transport_close(&transport);
            return NULL;
        }
        if (custom_hdr_count > 0) {
            memcpy(merged_hdrs, custom_hdrs,
                   custom_hdr_count * sizeof(xylem_http_hdr_t));
        }
        merged_hdrs[custom_hdr_count].name = "Cookie";
        merged_hdrs[custom_hdr_count].value = cookie_val;

        custom_hdrs = merged_hdrs;
        custom_hdr_count = merged_hdr_count;
    }

    /* Serialize request. */
    size_t req_len = 0;
    char* req_buf = http_req_serialize(
        cur_method, &parsed, cur_body, cur_body_len, cur_content_type,
        false, &req_len, custom_hdrs, custom_hdr_count);

    free(merged_hdrs);
    free(cookie_val);

    if (!req_buf) {
        _transport_close(&transport);
        return NULL;
    }

    /* Send request. */
    int wrc = _transport_write(&transport, req_buf, (int)req_len);
    free(req_buf);
    if (wrc != 0) {
        _transport_close(&transport);
        return NULL;
    }

    /* Read response. */
    xylem_http_res_t* res = (xylem_http_res_t*)calloc(1, sizeof(*res));
    if (!res) {
        _transport_close(&transport);
        return NULL;
    }

    _cli_parser_t cp;
    _cli_parser_init(&cp, res);

    char readbuf[4096];
    while (!cp.complete) {
        int n = _transport_read(&transport, readbuf, (int)sizeof(readbuf));
        if (n <= 0) {
            /* If we got headers, try llhttp_finish for EOF-terminated bodies. */
            if (n == 0) {
                llhttp_finish(&cp.parser);
                if (cp.complete) {
                    break;
                }
            }
            _cli_parser_destroy(&cp);
            _transport_close(&transport);
            xylem_http_res_destroy(res);
            return NULL;
        }

        llhttp_errno_t err = llhttp_execute(&cp.parser, readbuf, (size_t)n);
        if (err == HPE_PAUSED) {
            llhttp_resume(&cp.parser);
        } else if (err != HPE_OK) {
            _cli_parser_destroy(&cp);
            _transport_close(&transport);
            xylem_http_res_destroy(res);
            return NULL;
        }
    }

    /* Return connection to pool if keep-alive, otherwise close. */
    bool keep_alive = llhttp_should_keep_alive(&cp.parser) != 0;
    _cli_parser_destroy(&cp);

    if (keep_alive) {
        _pool_release(&parsed, &transport);
    } else {
        _transport_close(&transport);
    }

    /* Collect Set-Cookie headers into jar if provided. */
    if (opts && opts->cookie_jar) {
        _cookie_jar_collect(opts->cookie_jar, res->headers, res->header_count,
                            parsed.host, parsed.path);
    }

    /* Handle redirects (301, 302, 303, 307, 308). */
    int status = res->status_code;
    if (redirects_left > 0 &&
        (status == 301 || status == 302 || status == 303 ||
         status == 307 || status == 308)) {

        const char* location = http_header_find(res->headers, res->header_count,
                                                "Location");
        if (location) {
            http_url_t next_url;
            if (_resolve_redirect_url(location, &parsed, &next_url) == 0) {
                /* For 303: change method to GET and drop body. */
                if (status == 303) {
                    cur_method = "GET";
                    cur_body = NULL;
                    cur_body_len = 0;
                    cur_content_type = NULL;
                }
                /* For 301/302: conventionally change to GET (like browsers). */
                else if (status == 301 || status == 302) {
                    cur_method = "GET";
                    cur_body = NULL;
                    cur_body_len = 0;
                    cur_content_type = NULL;
                }
                /* For 307/308: preserve method and body. */

                xylem_http_res_destroy(res);
                parsed = next_url;
                redirects_left--;
                goto redirect_loop;
            }
        }
    }

    /* Auto-decompress if Content-Encoding: gzip. */
    const char* ce = http_header_find(res->headers, res->header_count,
                                      "Content-Encoding");
    if (ce && strstr(ce, "gzip") && res->body && res->body_len > 0) {
        /* Try progressively larger buffers: 4x, 8x, 16x of source. */
        size_t src_len = res->body_len;
        uint8_t* decompressed = NULL;
        int written = -1;
        size_t multipliers[] = {4, 8, 16, 32};
        for (size_t i = 0; i < 4; i++) {
            size_t dst_len = src_len * multipliers[i];
            uint8_t* buf = (uint8_t*)malloc(dst_len);
            if (!buf) {
                continue;
            }
            written = xylem_gzip_decompress(res->body, src_len, buf, dst_len);
            if (written >= 0) {
                decompressed = buf;
                break;
            }
            free(buf);
        }
        if (decompressed && written >= 0) {
            free(res->body);
            res->body = decompressed;
            res->body_len = (size_t)written;
            res->body_cap = (size_t)written;
        }
    }

    return res;
}

typedef struct {
    char*                   method;   /* NULL = match all methods */
    char*                   pattern;
    xylem_http_handler_fn_t handler;
    void*                   userdata;
} _route_entry_t;

typedef struct {
    xylem_http_middleware_fn_t fn;
    void*                     userdata;
} _middleware_entry_t;

struct xylem_http_router_s {
    _route_entry_t*      routes;
    size_t               route_count;
    size_t               route_cap;
    _middleware_entry_t* middlewares;
    size_t               mw_count;
    size_t               mw_cap;
};

xylem_http_router_t* xylem_http_router_create(void) {
    xylem_http_router_t* r =
        (xylem_http_router_t*)calloc(1, sizeof(xylem_http_router_t));
    return r;
}

void xylem_http_router_destroy(xylem_http_router_t* r) {
    if (!r) {
        return;
    }
    for (size_t i = 0; i < r->route_count; i++) {
        free(r->routes[i].method);
        free(r->routes[i].pattern);
    }
    free(r->routes);
    free(r->middlewares);
    free(r);
}

int xylem_http_router_add(xylem_http_router_t* r,
                          const char* method,
                          const char* pattern,
                          xylem_http_handler_fn_t handler,
                          void* userdata) {
    if (!r || !pattern || !handler) {
        return -1;
    }

    /* Grow routes array if needed. */
    if (r->route_count >= r->route_cap) {
        size_t new_cap = r->route_cap == 0 ? 8 : r->route_cap * 2;
        _route_entry_t* tmp = (_route_entry_t*)realloc(
            r->routes, new_cap * sizeof(_route_entry_t));
        if (!tmp) {
            return -1;
        }
        r->routes = tmp;
        r->route_cap = new_cap;
    }

    _route_entry_t* entry = &r->routes[r->route_count];
    entry->method = method ? strdup(method) : NULL;
    entry->pattern = strdup(pattern);
    entry->handler = handler;
    entry->userdata = userdata;

    if (!entry->pattern || (method && !entry->method)) {
        free(entry->method);
        free(entry->pattern);
        return -1;
    }

    r->route_count++;
    return 0;
}

int xylem_http_router_use(xylem_http_router_t* r,
                          xylem_http_middleware_fn_t mw,
                          void* userdata) {
    if (!r || !mw) {
        return -1;
    }

    /* Grow middleware array if needed. */
    if (r->mw_count >= r->mw_cap) {
        size_t new_cap = r->mw_cap == 0 ? 4 : r->mw_cap * 2;
        _middleware_entry_t* tmp = (_middleware_entry_t*)realloc(
            r->middlewares, new_cap * sizeof(_middleware_entry_t));
        if (!tmp) {
            return -1;
        }
        r->middlewares = tmp;
        r->mw_cap = new_cap;
    }

    r->middlewares[r->mw_count].fn = mw;
    r->middlewares[r->mw_count].userdata = userdata;
    r->mw_count++;
    return 0;
}

/**
 * Route match type for priority ordering.
 * Lower numeric value = higher priority.
 */
typedef enum {
    _MATCH_EXACT    = 0,
    _MATCH_PARAM    = 1,
    _MATCH_WILDCARD = 2,
    _MATCH_NONE     = 3,
} _match_type_t;

/**
 * Try to match a URL path against a route pattern.
 *
 * On success, populates params array with captured path parameters.
 * Returns the match type. _MATCH_NONE means no match.
 */
static _match_type_t _route_match(const char* pattern,
                                  const char* path,
                                  http_header_t** params,
                                  size_t* param_count,
                                  size_t* param_cap) {
    /* Wildcard: pattern ends with '*' -- prefix match */
    size_t plen = strlen(pattern);
    if (plen > 0 && pattern[plen - 1] == '*') {
        /* Match prefix up to the '*'. */
        size_t prefix_len = plen - 1;
        if (strncmp(path, pattern, prefix_len) == 0) {
            return _MATCH_WILDCARD;
        }
        return _MATCH_NONE;
    }

    /* Try exact or parameterized match by walking segments. */
    const char* pp = pattern;
    const char* up = path;
    _match_type_t result = _MATCH_EXACT;

    while (*pp && *up) {
        if (*pp == ':') {
            /* Path parameter segment. */
            result = _MATCH_PARAM;

            /* Extract param name (up to '/' or end). */
            pp++; /* skip ':' */
            const char* name_start = pp;
            while (*pp && *pp != '/') {
                pp++;
            }
            size_t name_len = (size_t)(pp - name_start);

            /* Extract value from URL (up to '/' or end). */
            const char* val_start = up;
            while (*up && *up != '/') {
                up++;
            }
            size_t val_len = (size_t)(up - val_start);

            /* Store the captured parameter. */
            if (params) {
                http_header_add(params, param_count, param_cap,
                                name_start, name_len, val_start, val_len);
            }
        } else {
            /* Literal character -- must match exactly. */
            if (*pp != *up) {
                return _MATCH_NONE;
            }
            pp++;
            up++;
        }
    }

    /* Both pattern and path must be fully consumed for a match. */
    if (*pp != '\0' || *up != '\0') {
        return _MATCH_NONE;
    }

    return result;
}

static const struct { const char* ext; const char* mime; } _mime_map[] = {
    {".html", "text/html"},       {".htm",  "text/html"},
    {".css",  "text/css"},        {".js",   "application/javascript"},
    {".json", "application/json"},{".png",  "image/png"},
    {".jpg",  "image/jpeg"},      {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},       {".svg",  "image/svg+xml"},
    {".ico",  "image/x-icon"},    {".woff2","font/woff2"},
    {".woff", "font/woff"},       {".ttf",  "font/ttf"},
    {".txt",  "text/plain"},      {".xml",  "application/xml"},
    {".pdf",  "application/pdf"}, {".wasm", "application/wasm"},
    {NULL, NULL}
};

static const char* _mime_lookup(const char* path) {
    const char* dot = NULL;
    /* Find last '.' in path. */
    for (const char* p = path; *p; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    if (!dot) {
        return "application/octet-stream";
    }
    for (int i = 0; _mime_map[i].ext != NULL; i++) {
        /* Case-insensitive extension compare. */
        const char* a = dot;
        const char* b = _mime_map[i].ext;
        bool match = true;
        while (*a && *b) {
            char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
            char cb = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
            if (ca != cb) {
                match = false;
                break;
            }
            a++;
            b++;
        }
        if (match && *a == '\0' && *b == '\0') {
            return _mime_map[i].mime;
        }
    }
    return "application/octet-stream";
}

typedef struct {
    char  root[2048];
    char  index_file[64];
    int   max_age;
    bool  precompressed;
    size_t prefix_len;
} _static_ctx_t;

/**
 * Check if a path contains traversal sequences (e.g. "..").
 * Returns true if the path is unsafe.
 */
static bool _path_has_traversal(const char* path) {
    const char* p = path;
    while (*p) {
        /* Look for ".." as a path segment. */
        if (p[0] == '.' && p[1] == '.') {
            /* Check it's bounded by '/', '\', or start/end. */
            bool left_ok = (p == path || p[-1] == '/' || p[-1] == '\\');
            bool right_ok = (p[2] == '\0' || p[2] == '/' || p[2] == '\\');
            if (left_ok && right_ok) {
                return true;
            }
        }
        p++;
    }
    return false;
}

static void _static_handler(xylem_http_res_t* res,
                            xylem_http_req_t* req,
                            void* ud) {
    _static_ctx_t* ctx = (_static_ctx_t*)ud;
    const char* method = xylem_http_req_method(req);
    const char* url = xylem_http_req_url(req);

    /* Only GET/HEAD. */
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        xylem_http_res_set_status(res, 405);
        xylem_http_res_set_header(res, "Allow", "GET, HEAD");
        xylem_http_res_write(res, "Method Not Allowed", 18);
        return;
    }

    /* Strip query string. */
    const char* qmark = strchr(url, '?');
    size_t path_len = qmark ? (size_t)(qmark - url) : strlen(url);

    /* Extract the suffix after the prefix. */
    const char* suffix = url + ctx->prefix_len;
    size_t suffix_len = path_len - ctx->prefix_len;

    /* Path traversal check. */
    /* Build a temporary NUL-terminated suffix for checking. */
    char suffix_buf[4096];
    if (suffix_len >= sizeof(suffix_buf)) {
        xylem_http_res_set_status(res, 414);
        xylem_http_res_write(res, "URI Too Long", 12);
        return;
    }
    memcpy(suffix_buf, suffix, suffix_len);
    suffix_buf[suffix_len] = '\0';

    if (_path_has_traversal(suffix_buf)) {
        xylem_http_res_set_status(res, 403);
        xylem_http_res_write(res, "Forbidden", 9);
        return;
    }

    /* Build the filesystem path: root + suffix. */
    char fs_path[4096];
    size_t root_len = strlen(ctx->root);

    /* Remove trailing slash from root if present. */
    if (root_len > 0 && (ctx->root[root_len - 1] == '/' ||
                         ctx->root[root_len - 1] == '\\')) {
        root_len--;
    }

    /* If suffix is empty or ends with '/', append the index file. */
    bool is_dir_request = (suffix_len == 0) ||
                          (suffix_len > 0 && suffix_buf[suffix_len - 1] == '/');

    if (is_dir_request) {
        int n = snprintf(fs_path, sizeof(fs_path), "%.*s/%s%s",
                         (int)root_len, ctx->root, suffix_buf,
                         ctx->index_file);
        if (n < 0 || (size_t)n >= sizeof(fs_path)) {
            xylem_http_res_set_status(res, 414);
            xylem_http_res_write(res, "URI Too Long", 12);
            return;
        }
    } else {
        int n = snprintf(fs_path, sizeof(fs_path), "%.*s/%s",
                         (int)root_len, ctx->root, suffix_buf);
        if (n < 0 || (size_t)n >= sizeof(fs_path)) {
            xylem_http_res_set_status(res, 414);
            xylem_http_res_write(res, "URI Too Long", 12);
            return;
        }
    }

    /* Normalize path separators to the platform style for fopen. */
#ifdef _WIN32
    for (char* p = fs_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    /* Check for precompressed .gz variant. */
    bool serve_gzip = false;
    char gz_path[4096];

    if (ctx->precompressed) {
        /* Check if client accepts gzip. */
        const char* ae = xylem_http_req_header(req, "Accept-Encoding");
        if (ae && strstr(ae, "gzip")) {
            snprintf(gz_path, sizeof(gz_path), "%s.gz", fs_path);
            FILE* gz_f = fopen(gz_path, "rb");
            if (gz_f) {
                fclose(gz_f);
                serve_gzip = true;
            }
        }
    }

    /* Open the file. */
    const char* open_path = serve_gzip ? gz_path : fs_path;
    FILE* f = fopen(open_path, "rb");
    if (!f) {
        xylem_http_res_set_status(res, 404);
        xylem_http_res_write(res, "Not Found", 9);
        return;
    }

    /* Get file size. */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < 0) {
        fclose(f);
        xylem_http_res_set_status(res, 500);
        xylem_http_res_write(res, "Internal Server Error", 21);
        return;
    }

    /* Read file content. */
    uint8_t* content = (uint8_t*)malloc((size_t)fsize);
    if (!content) {
        fclose(f);
        xylem_http_res_set_status(res, 500);
        xylem_http_res_write(res, "Internal Server Error", 21);
        return;
    }

    size_t nread = fread(content, 1, (size_t)fsize, f);
    fclose(f);

    if (nread != (size_t)fsize) {
        free(content);
        xylem_http_res_set_status(res, 500);
        xylem_http_res_write(res, "Internal Server Error", 21);
        return;
    }

    /* Set Content-Type based on the original path (not .gz). */
    const char* mime = _mime_lookup(fs_path);
    xylem_http_res_set_header(res, "Content-Type", mime);

    /* Set Cache-Control. */
    if (ctx->max_age > 0) {
        char cache_val[64];
        snprintf(cache_val, sizeof(cache_val), "max-age=%d", ctx->max_age);
        xylem_http_res_set_header(res, "Cache-Control", cache_val);
    }

    /* Set Content-Encoding if serving precompressed. */
    if (serve_gzip) {
        xylem_http_res_set_header(res, "Content-Encoding", "gzip");
    }

    /* For HEAD, send headers only (write with length 0 won't produce body). */
    if (strcmp(method, "HEAD") == 0) {
        xylem_http_res_set_status(res, 200);
        /* Don't write body for HEAD. */
    } else {
        xylem_http_res_set_status(res, 200);
        xylem_http_res_write(res, content, nread);
    }

    free(content);
}

int xylem_http_static_serve(xylem_http_router_t* r,
                            const char* prefix,
                            const xylem_http_static_opts_t* opts) {
    if (!r || !prefix || !opts || !opts->root) {
        return -1;
    }

    _static_ctx_t* ctx = (_static_ctx_t*)calloc(1, sizeof(_static_ctx_t));
    if (!ctx) {
        return -1;
    }

    /* Copy root, stripping trailing slash. */
    size_t root_len = strlen(opts->root);
    if (root_len >= sizeof(ctx->root)) {
        root_len = sizeof(ctx->root) - 1;
    }
    memcpy(ctx->root, opts->root, root_len);
    ctx->root[root_len] = '\0';

    /* Index file. */
    const char* idx = opts->index_file ? opts->index_file : "index.html";
    size_t idx_len = strlen(idx);
    if (idx_len >= sizeof(ctx->index_file)) {
        idx_len = sizeof(ctx->index_file) - 1;
    }
    memcpy(ctx->index_file, idx, idx_len);
    ctx->index_file[idx_len] = '\0';

    ctx->max_age = opts->max_age;
    ctx->precompressed = opts->precompressed;

    /* Compute the prefix length for stripping from URLs.
     * The wildcard route pattern is "prefix*" so it matches URLs
     * starting with "prefix". We store prefix_len to skip past it. */
    ctx->prefix_len = strlen(prefix);

    /* Register a wildcard route: prefix + "*". */
    char pattern[2048];
    int n = snprintf(pattern, sizeof(pattern), "%s*", prefix);
    if (n < 0 || (size_t)n >= sizeof(pattern)) {
        free(ctx);
        return -1;
    }

    /* Register for all methods (NULL) so the handler can send 405 for
     * non-GET/HEAD methods itself. */
    return xylem_http_router_add(r, NULL, pattern, _static_handler, ctx);
}

int xylem_http_router_dispatch(xylem_http_router_t* r,
                               xylem_http_res_t* res,
                               xylem_http_req_t* req) {
    if (!r || !res || !req) {
        return -1;
    }

    const char* method = req->method;
    const char* url = req->url;

    /* Strip query string for path matching. */
    const char* qmark = strchr(url, '?');
    char* path = NULL;
    if (qmark) {
        size_t path_len = (size_t)(qmark - url);
        path = (char*)malloc(path_len + 1);
        if (!path) {
            xylem_http_res_set_status(res, 500);
            xylem_http_res_write(res, "Internal Server Error", 21);
            return -1;
        }
        memcpy(path, url, path_len);
        path[path_len] = '\0';
    } else {
        path = strdup(url);
        if (!path) {
            xylem_http_res_set_status(res, 500);
            xylem_http_res_write(res, "Internal Server Error", 21);
            return -1;
        }
    }

    /* Find best matching route. */
    _match_type_t best_type = _MATCH_NONE;
    size_t best_idx = 0;
    size_t best_plen = 0;
    bool best_method_specific = false;

    for (size_t i = 0; i < r->route_count; i++) {
        _route_entry_t* route = &r->routes[i];

        /* Check method match. */
        bool method_specific = (route->method != NULL);
        if (method_specific && strcmp(route->method, method) != 0) {
            continue;
        }

        /* Try pattern match (without capturing params yet). */
        _match_type_t mt = _route_match(route->pattern, path,
                                        NULL, NULL, NULL);
        if (mt == _MATCH_NONE) {
            continue;
        }

        size_t plen = strlen(route->pattern);

        /* Priority: exact > param > wildcard.
         * Among same type: longer pattern wins.
         * Among same type+length: specific method beats wildcard method. */
        bool better = false;
        if (mt < best_type) {
            better = true;
        } else if (mt == best_type) {
            if (plen > best_plen) {
                better = true;
            } else if (plen == best_plen) {
                if (method_specific && !best_method_specific) {
                    better = true;
                }
            }
        }

        if (better) {
            best_type = mt;
            best_idx = i;
            best_plen = plen;
            best_method_specific = method_specific;
        }
    }

    if (best_type == _MATCH_NONE) {
        /* No route matched -- send 404. */
        free(path);
        xylem_http_res_set_status(res, 404);
        xylem_http_res_write(res, "Not Found", 9);
        return -1;
    }

    /* Capture path parameters from the matched route. */
    _route_entry_t* matched = &r->routes[best_idx];
    if (best_type == _MATCH_PARAM) {
        _route_match(matched->pattern, path,
                     &req->params, &req->param_count, &req->param_cap);
    }

    free(path);

    /* Run middleware chain. */
    for (size_t i = 0; i < r->mw_count; i++) {
        if (r->middlewares[i].fn(res, req, r->middlewares[i].userdata) != 0) {
            /* Middleware aborted -- response already sent. */
            return 0;
        }
    }

    /* Call matched handler. */
    matched->handler(res, req, matched->userdata);
    return 0;
}
