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

/**
 * Coroutine-based HTTP server and client implementation.
 *
 * Server: xylem_http_listen spawns an accept coroutine. Each accepted
 * connection gets its own coroutine that reads bytes, parses HTTP
 * requests via llhttp, calls the user handler, and auto-finalizes
 * the response.
 *
 * Client: xylem_http_get/post/put/delete/patch dial TCP (or TLS for
 * https://), serialize the request using http_req_serialize(), read
 * the response via llhttp, and return xylem_http_res_t*.
 */

#include "xylem/net/xylem-http.h"
#include "xylem/net/xylem-tcp.h"
#include "xylem/xylem-utils.h"

#include "http-common.h"
#include "net/http/llhttp/llhttp.h"
#include "runtime/runtime.h"

#ifdef XYLEM_ENABLE_TLS
#include "xylem/net/xylem-tls.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── Transport abstraction (inline dispatch, not vtable) ──────── */

typedef enum {
    TRANSPORT_TCP,
    TRANSPORT_TLS,
} _transport_kind_t;

typedef struct {
    _transport_kind_t kind;
    union {
        xylem_tcp_conn_t* tcp;
#ifdef XYLEM_ENABLE_TLS
        xylem_tls_conn_t* tls;
#endif
    } conn;
} _transport_t;

static inline int _transport_read(_transport_t* t, void* buf, int len) {
    if (t->kind == TRANSPORT_TCP) {
        return xylem_tcp_read(t->conn.tcp, buf, len);
    }
#ifdef XYLEM_ENABLE_TLS
    if (t->kind == TRANSPORT_TLS) {
        return xylem_tls_read(t->conn.tls, buf, len);
    }
#endif
    return -1;
}

static inline int _transport_write(_transport_t* t, const void* data, int len) {
    if (t->kind == TRANSPORT_TCP) {
        return xylem_tcp_write(t->conn.tcp, data, len);
    }
#ifdef XYLEM_ENABLE_TLS
    if (t->kind == TRANSPORT_TLS) {
        return xylem_tls_write(t->conn.tls, data, len);
    }
#endif
    return -1;
}

static inline void _transport_close(_transport_t* t) {
    if (t->kind == TRANSPORT_TCP) {
        xylem_tcp_close(t->conn.tcp);
    }
#ifdef XYLEM_ENABLE_TLS
    else if (t->kind == TRANSPORT_TLS) {
        xylem_tls_close(t->conn.tls);
    }
#endif
}

/* ─── Server structures ────────────────────────────────────────── */

struct xylem_http_srv_s {
    xylem_tcp_listener_t*   listener;
    xylem_http_handler_fn_t handler;
    void*                   userdata;
    uint16_t                port;
};

/* Per-connection context for the server-side coroutine. */
typedef struct {
    xylem_http_srv_t* srv;
    _transport_t      transport;
} _srv_conn_ctx_t;

/* ─── Request (server-side parsed request) ─────────────────────── */

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
};

/* ─── Response (dual-purpose: server writer + client result) ───── */

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
    _transport_t*  _transport;  /* Non-NULL only while handler runs */
    bool           _headers_sent;
};

/* ─── Request accessors ────────────────────────────────────────── */

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

/* ─── Response accessors ───────────────────────────────────────── */

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

/* ─── Server-side response writer ─────────────────────────────── */

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

/**
 * Flush status line + headers with Transfer-Encoding: chunked.
 * Called on the first xylem_http_res_write().
 */
static int _flush_headers(xylem_http_res_t* res) {
    if (!res->_transport) {
        return -1;
    }

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

    /* Send chunk: "<hex-len>\r\n<data>\r\n" */
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

/**
 * Send the terminating chunk "0\r\n\r\n" after the handler returns.
 * If no writes happened, flush empty response first.
 */
static void _finalize_response(xylem_http_res_t* res) {
    if (!res->_transport) {
        return;
    }
    if (!res->_headers_sent) {
        /* Handler returned without writing -- send headers + empty body. */
        _flush_headers(res);
    }
    /* Send terminating chunk. */
    _transport_write(res->_transport, "0\r\n\r\n", 5);
}

/* ─── Server: llhttp callbacks for request parsing ─────────────── */

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
    free(sp->req.body);
    free(sp->cur_hdr_name);
}

/* ─── Server: per-connection coroutine ─────────────────────────── */

static void _srv_conn_coroutine(void* arg) {
    _srv_conn_ctx_t* ctx = (_srv_conn_ctx_t*)arg;
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

/* ─── Server: accept-loop coroutine ───────────────────────────── */

static void _srv_accept_coroutine(void* arg) {
    xylem_http_srv_t* srv = (xylem_http_srv_t*)arg;

    for (;;) {
        xylem_tcp_conn_t* conn = xylem_tcp_accept(srv->listener);
        if (!conn) {
            break; /* listener closed */
        }

        _srv_conn_ctx_t* ctx = (_srv_conn_ctx_t*)calloc(1, sizeof(*ctx));
        if (!ctx) {
            xylem_tcp_close(conn);
            continue;
        }
        ctx->srv = srv;
        ctx->transport.kind = TRANSPORT_TCP;
        ctx->transport.conn.tcp = conn;

        runtime_spawn(_srv_conn_coroutine, ctx);
    }
}

/* ─── Server: public API ───────────────────────────────────────── */

xylem_http_srv_t* xylem_http_listen(
    const char*                  host,
    uint16_t                     port,
    xylem_http_handler_fn_t      handler,
    void*                        userdata,
    const xylem_http_srv_opts_t* opts) {
    (void)opts; /* TODO: TLS, max_body_size, idle_timeout */

    if (!handler) {
        return NULL;
    }

    xylem_tcp_listener_t* ln = xylem_tcp_listen(host, port, NULL);
    if (!ln) {
        return NULL;
    }

    xylem_http_srv_t* srv = (xylem_http_srv_t*)calloc(1, sizeof(*srv));
    if (!srv) {
        xylem_tcp_close_listener(ln);
        return NULL;
    }
    srv->listener = ln;
    srv->handler  = handler;
    srv->userdata = userdata;

    /* Discover the actual port (useful if port was 0). */
    char host_buf[46];
    uint16_t actual_port = 0;
    xylem_tcp_listener_addr(ln, host_buf, sizeof(host_buf), &actual_port);
    srv->port = actual_port;

    /* Spawn the accept loop. */
    runtime_spawn(_srv_accept_coroutine, srv);

    return srv;
}

void xylem_http_close_server(xylem_http_srv_t* srv) {
    if (!srv) {
        return;
    }
    xylem_tcp_close_listener(srv->listener);
    /* Note: in-flight coroutines will exit when read returns <=0. */
    free(srv);
}

uint16_t xylem_http_server_port(xylem_http_srv_t* srv) {
    return srv ? srv->port : 0;
}

/* ─── Client: llhttp callbacks for response parsing ────────────── */

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

/* ─── Client: core request execution ──────────────────────────── */

static xylem_http_res_t* _client_do_request(
    const char*              method,
    const char*              url,
    const void*              body,
    size_t                   body_len,
    const char*              content_type,
    const xylem_http_opts_t* opts) {

    if (!url) {
        return NULL;
    }

    /* Parse URL. */
    http_url_t parsed;
    if (http_url_parse(url, &parsed) != 0) {
        return NULL;
    }

    bool use_tls = (strcmp(parsed.scheme, "https") == 0);

    /* Connect. */
    _transport_t transport;
    memset(&transport, 0, sizeof(transport));

    if (use_tls) {
#ifdef XYLEM_ENABLE_TLS
        xylem_tls_ctx_t* tls_ctx = NULL;
        if (opts && opts->tls_ctx) {
            tls_ctx = opts->tls_ctx;
        } else {
            tls_ctx = xylem_tls_ctx_create();
            if (!tls_ctx) {
                return NULL;
            }
        }
        xylem_tls_opts_t tls_opts = {0};
        tls_opts.server_name = parsed.host;
        tls_opts.handshake_timeout_ms = 10000;
        xylem_tls_conn_t* c = xylem_tls_dial(
            parsed.host, parsed.port, tls_ctx, &tls_opts);
        if (!opts || !opts->tls_ctx) {
            xylem_tls_ctx_destroy(tls_ctx);
        }
        if (!c) {
            return NULL;
        }
        transport.kind = TRANSPORT_TLS;
        transport.conn.tls = c;
#else
        return NULL; /* TLS not available */
#endif
    } else {
        uint64_t timeout = 10000;
        if (opts && opts->timeout_ms > 0) {
            timeout = opts->timeout_ms;
        }
        xylem_tcp_conn_t* c = xylem_tcp_dial(
            parsed.host, parsed.port, timeout, NULL);
        if (!c) {
            return NULL;
        }
        transport.kind = TRANSPORT_TCP;
        transport.conn.tcp = c;
    }

    /* Set deadlines. */
    uint64_t deadline_ms = 0;
    if (opts && opts->timeout_ms > 0) {
        deadline_ms = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                      + opts->timeout_ms;
    } else {
        deadline_ms = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 30000;
    }
    if (transport.kind == TRANSPORT_TCP) {
        xylem_tcp_set_read_deadline(transport.conn.tcp, deadline_ms);
        xylem_tcp_set_write_deadline(transport.conn.tcp, deadline_ms);
    }
#ifdef XYLEM_ENABLE_TLS
    else {
        xylem_tls_set_read_deadline(transport.conn.tls, deadline_ms);
        xylem_tls_set_write_deadline(transport.conn.tls, deadline_ms);
    }
#endif

    /* Serialize request. */
    const xylem_http_hdr_t* custom_hdrs = NULL;
    size_t custom_hdr_count = 0;
    if (opts) {
        custom_hdrs = opts->headers;
        custom_hdr_count = opts->header_count;
    }

    size_t req_len = 0;
    char* req_buf = http_req_serialize(
        method, &parsed, body, body_len, content_type,
        false, &req_len, custom_hdrs, custom_hdr_count);
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

    _cli_parser_destroy(&cp);
    _transport_close(&transport);
    return res;
}

/* ─── Client: public API ───────────────────────────────────────── */

xylem_http_res_t* xylem_http_get(const char* url,
                                 const xylem_http_opts_t* opts) {
    return _client_do_request("GET", url, NULL, 0, NULL, opts);
}

xylem_http_res_t* xylem_http_post(const char* url,
                                  const void* body, size_t body_len,
                                  const char* content_type,
                                  const xylem_http_opts_t* opts) {
    return _client_do_request("POST", url, body, body_len, content_type, opts);
}

xylem_http_res_t* xylem_http_put(const char* url,
                                 const void* body, size_t body_len,
                                 const char* content_type,
                                 const xylem_http_opts_t* opts) {
    return _client_do_request("PUT", url, body, body_len, content_type, opts);
}

xylem_http_res_t* xylem_http_delete(const char* url,
                                    const xylem_http_opts_t* opts) {
    return _client_do_request("DELETE", url, NULL, 0, NULL, opts);
}

xylem_http_res_t* xylem_http_patch(const char* url,
                                   const void* body, size_t body_len,
                                   const char* content_type,
                                   const xylem_http_opts_t* opts) {
    return _client_do_request("PATCH", url, body, body_len, content_type, opts);
}
