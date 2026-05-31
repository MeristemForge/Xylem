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

/*
 * HTTP module core: protocol engine + public API + scheme dispatch.
 *
 * The engine (request parsing, the server connection coroutine, the client
 * request loop, connection pooling, response assembly) is transport-agnostic:
 * it speaks only through http_transport_t. Plain TCP and TLS transports are
 * built by separate factories (transport-tcp.c / transport-tls.c); choosing
 * between them happens here at runtime, never via a macro inside the engine.
 */

#include "xylem/net/http/xylem-http.h"
#include "xylem/encoding/xylem-gzip.h"
#include "xylem/sync/xylem-mutex.h"

#include "http-internal.h"
#include "platform/platform-io.h"
#include "runtime/runtime.h"

#include "net/http/llhttp/llhttp.h"

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

#define HTTP_IO_BUF_SIZE        4096
#define HTTP_MAX_PATH           4096
#define POOL_MAX_IDLE_PER_HOST  2
#define POOL_IDLE_TIMEOUT_MS    90000
#define POOL_MAX_HOSTS          100

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

static _pool_entry_t  _pool[POOL_MAX_HOSTS];
static size_t         _pool_size;
static xylem_mutex_t* _pool_mu;

static void _pool_make_key(const http_url_t* url, char* key, size_t key_size) {
    snprintf(key, key_size, "%s:%u:%s", url->host, (unsigned)url->port,
             url->scheme);
}

static _pool_entry_t* _pool_find_entry(const char* key) {
    for (size_t i = 0; i < _pool_size; i++) {
        if (strcmp(_pool[i].key, key) == 0) {
            return &_pool[i];
        }
    }
    return NULL;
}

static void _pool_ensure_mu(void) {
    if (!_pool_mu) {
        _pool_mu = xylem_mutex_create();
    }
}

/* Evicts stale connections while searching. */
static bool _pool_acquire(const http_url_t* url, http_transport_t* out) {
    _pool_ensure_mu();
    xylem_mutex_lock(_pool_mu);

    char key[320];
    _pool_make_key(url, key, sizeof(key));

    _pool_entry_t* entry = _pool_find_entry(key);
    if (!entry || entry->count == 0) {
        xylem_mutex_unlock(_pool_mu);
        return false;
    }

    uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);

    while (entry->count > 0) {
        size_t idx = entry->count - 1;
        _pool_idle_conn_t* ic = &entry->conns[idx];

        if (now - ic->idle_since > POOL_IDLE_TIMEOUT_MS) {
            _transport_close(&ic->transport);
            entry->count--;
            continue;
        }

        *out = ic->transport;
        entry->count--;
        xylem_mutex_unlock(_pool_mu);
        return true;
    }

    xylem_mutex_unlock(_pool_mu);
    return false;
}

/* Evicts oldest if pool for this host is full. */
static void _pool_release(const http_url_t* url, http_transport_t* t) {
    _pool_ensure_mu();
    xylem_mutex_lock(_pool_mu);

    char key[320];
    _pool_make_key(url, key, sizeof(key));

    _pool_entry_t* entry = _pool_find_entry(key);
    if (!entry) {
        if (_pool_size >= POOL_MAX_HOSTS) {
            xylem_mutex_unlock(_pool_mu);
            _transport_close(t);
            return;
        }
        entry = &_pool[_pool_size++];
        memcpy(entry->key, key, strlen(key) + 1);
        entry->conns = NULL;
        entry->count = 0;
        entry->cap = 0;
    }

    if (entry->count >= POOL_MAX_IDLE_PER_HOST) {
        _transport_close(&entry->conns[0].transport);
        memmove(&entry->conns[0], &entry->conns[1],
                (entry->count - 1) * sizeof(_pool_idle_conn_t));
        entry->count--;
    }

    if (entry->count >= entry->cap) {
        size_t new_cap = entry->cap == 0 ? 4 : entry->cap * 2;
        if (new_cap > POOL_MAX_IDLE_PER_HOST) {
            new_cap = POOL_MAX_IDLE_PER_HOST;
        }
        _pool_idle_conn_t* tmp = (_pool_idle_conn_t*)realloc(
            entry->conns, new_cap * sizeof(_pool_idle_conn_t));
        if (!tmp) {
            xylem_mutex_unlock(_pool_mu);
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
    xylem_mutex_unlock(_pool_mu);
}


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

int xylem_http_req_headers(const xylem_http_req_t* req,
                           const xylem_http_hdr_t** headers,
                           size_t* count) {
    if (!req || !headers || !count) {
        return -1;
    }
    *headers = (const xylem_http_hdr_t*)req->headers;
    *count   = req->header_count;
    return 0;
}

const void* xylem_http_req_body(const xylem_http_req_t* req) {
    return req ? (const void*)req->body : NULL;
}

size_t xylem_http_req_body_len(const xylem_http_req_t* req) {
    return req ? req->body_len : 0;
}

int xylem_http_req_remote_addr(const xylem_http_req_t* req,
                               char* host, size_t host_len,
                               uint16_t* port) {
    if (!req) {
        return -1;
    }
    if (host && host_len > 0) {
        snprintf(host, host_len, "%s", req->remote_host);
    }
    if (port) {
        *port = req->remote_port;
    }
    return 0;
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


static int _emit_response_head(xylem_http_res_t* res,
                               const char* extra_hdr) {
    if (!res->_transport) {
        return -1;
    }

    int status = res->status_code ? res->status_code : 200;
    const char* reason = http_reason_phrase(status);
    size_t extra_len = extra_hdr ? strlen(extra_hdr) : 0;

    size_t est = 64 + strlen(reason) + extra_len;
    for (size_t i = 0; i < res->header_count; i++) {
        est += strlen(res->headers[i].name) + strlen(res->headers[i].value) + 4;
    }
    est += 2;

    char* buf = (char*)malloc(est);
    if (!buf) {
        return -1;
    }

    int off = snprintf(buf, est, "HTTP/1.1 %d %s\r\n", status, reason);

    for (size_t i = 0; i < res->header_count; i++) {
        off += snprintf(buf + off, est - (size_t)off, "%s: %s\r\n",
                        res->headers[i].name, res->headers[i].value);
    }
    if (extra_hdr) {
        memcpy(buf + off, extra_hdr, extra_len);
        off += (int)extra_len;
    }
    buf[off++] = '\r';
    buf[off++] = '\n';

    int rc = _transport_write(res->_transport, buf, off);
    free(buf);
    return rc;
}

/* Lazily called on first write; emits chunked encoding header. */
static int _flush_headers(xylem_http_res_t* res) {
    int rc = _emit_response_head(res, "Transfer-Encoding: chunked\r\n");
    if (rc != 0) {
        return -1;
    }
    res->_headers_sent = true;
    return 0;
}

static int _write_chunk(xylem_http_res_t* res, const void* data, size_t len) {
    char hdr[24];
    int hdr_len = snprintf(hdr, sizeof(hdr), "%zx\r\n", len);

    /* Small chunks: single write avoids multiple syscalls. */
    size_t frame_len = (size_t)hdr_len + len + 2;
    if (frame_len <= HTTP_IO_BUF_SIZE) {
        char buf[HTTP_IO_BUF_SIZE];
        memcpy(buf, hdr, (size_t)hdr_len);
        memcpy(buf + hdr_len, data, len);
        buf[hdr_len + (int)len] = '\r';
        buf[hdr_len + (int)len + 1] = '\n';
        return _transport_write(res->_transport, buf, (int)frame_len);
    }

    if (_transport_write(res->_transport, hdr, hdr_len) != 0) {
        return -1;
    }
    if (_transport_write(res->_transport, data, (int)len) != 0) {
        return -1;
    }
    return _transport_write(res->_transport, "\r\n", 2);
}


int xylem_http_res_write(xylem_http_res_t* res,
                         const void* data, size_t len) {
    if (!res || !res->_transport) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    /* Already streaming (chunked mode). */
    if (res->_headers_sent) {
        return _write_chunk(res, data, len);
    }

    /* First write: buffer it for potential Content-Length mode. */
    if (!res->_body_buf) {
        res->_body_buf = (uint8_t*)malloc(len);
        if (!res->_body_buf) {
            return -1;
        }
        memcpy(res->_body_buf, data, len);
        res->_body_buf_len = len;
        return 0;
    }

    /* Second write: switch to chunked mode. */
    if (_flush_headers(res) != 0) {
        return -1;
    }

    /* Write buffered data as first chunk. */
    if (res->_body_buf_len > 0) {
        if (_write_chunk(res, res->_body_buf, res->_body_buf_len) != 0) {
            free(res->_body_buf);
            res->_body_buf = NULL;
            res->_body_buf_len = 0;
            return -1;
        }
    }
    free(res->_body_buf);
    res->_body_buf = NULL;
    res->_body_buf_len = 0;

    /* Write current data. */
    return _write_chunk(res, data, len);
}

static int _emit_fixed_headers(xylem_http_res_t* res) {
    return _emit_response_head(res, NULL);
}

static void _finalize_response(xylem_http_res_t* res) {
    if (!res->_transport) {
        return;
    }

    /* Single-write response: use Content-Length mode. */
    if (!res->_headers_sent && res->_body_buf) {
        char cl_str[24];
        int cl_n = snprintf(cl_str, sizeof(cl_str), "%zu", res->_body_buf_len);
        http_header_add(&res->headers, &res->header_count, &res->header_cap,
                        "Content-Length", 14, cl_str, (size_t)cl_n);

        _emit_fixed_headers(res);

        if (res->_body_buf_len > 0) {
            _transport_write(res->_transport, res->_body_buf,
                             (int)res->_body_buf_len);
        }
        free(res->_body_buf);
        res->_body_buf = NULL;
        res->_body_buf_len = 0;
        return;
    }

    /* No-body response (status set but no write called). */
    if (!res->_headers_sent && !res->_body_buf) {
        http_header_add(&res->headers, &res->header_count, &res->header_cap,
                        "Content-Length", 14, "0", 1);
        _emit_fixed_headers(res);
        return;
    }

    /* Chunked mode: finalize with 0-chunk. */
    if (!res->_headers_sent) {
        _flush_headers(res);
    }

    _transport_write(res->_transport, "0\r\n\r\n", 5);
}

int xylem_http_res_upgrade(xylem_http_res_t* res, void** transport) {
    if (!res || !res->_transport || !transport) {
        return -1;
    }
    if (res->_headers_sent) {
        return -1;
    }

    const char* resp = "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n";
    int n = res->_transport->write(res->_transport->conn,
                                   resp, (int)strlen(resp));
    if (n < 0) {
        return -1;
    }

    for (size_t i = 0; i < res->header_count; i++) {
        char hdr[512];
        int hlen = snprintf(hdr, sizeof(hdr), "%s: %s\r\n",
                            res->headers[i].name, res->headers[i].value);
        if (hlen > 0) {
            res->_transport->write(res->_transport->conn, hdr, hlen);
        }
    }

    res->_transport->write(res->_transport->conn, "\r\n", 2);

    /* Detach: caller now owns the connection. */
    *transport = res->_transport;
    res->_transport = NULL;
    res->_headers_sent = true;

    return 0;
}

static int _hdr_field_append(char** name, size_t* name_len, size_t* name_cap,
                             const char* at, size_t len) {
    size_t needed = *name_len + len + 1;
    if (needed > *name_cap) {
        size_t new_cap = *name_cap ? *name_cap * 2 : 64;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        char* tmp = (char*)realloc(*name, new_cap);
        if (!tmp) {
            return -1;
        }
        *name = tmp;
        *name_cap = new_cap;
    }
    memcpy(*name + *name_len, at, len);
    *name_len += len;
    (*name)[*name_len] = '\0';
    return 0;
}

static int _body_append(uint8_t** body, size_t* body_len, size_t* body_cap,
                        const char* at, size_t len) {
    size_t needed = *body_len + len;
    if (needed > *body_cap) {
        size_t new_cap = *body_cap ? *body_cap * 2 : 4096;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        uint8_t* tmp = (uint8_t*)realloc(*body, new_cap);
        if (!tmp) {
            return -1;
        }
        *body = tmp;
        *body_cap = new_cap;
    }
    memcpy(*body + *body_len, at, len);
    *body_len += len;
    return 0;
}

typedef struct {
    llhttp_t          parser;
    llhttp_settings_t settings;
    xylem_http_req_t  req;
    char*             cur_hdr_name;
    size_t            cur_hdr_name_len;
    size_t            cur_hdr_name_cap;
    size_t            url_cap;
    http_transport_t* transport;
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
    size_t needed = ctx->req.url_len + len + 1;
    if (needed > ctx->url_cap) {
        size_t new_cap = ctx->url_cap ? ctx->url_cap * 2 : 64;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        char* tmp = (char*)realloc(ctx->req.url, new_cap);
        if (!tmp) {
            return -1;
        }
        ctx->req.url = tmp;
        ctx->url_cap = new_cap;
    }
    memcpy(ctx->req.url + ctx->req.url_len, at, len);
    ctx->req.url_len += len;
    ctx->req.url[ctx->req.url_len] = '\0';
    return 0;
}

static int _srv_on_header_field(llhttp_t* p, const char* at, size_t len) {
    _srv_parser_t* ctx = (_srv_parser_t*)p->data;
    return _hdr_field_append(&ctx->cur_hdr_name, &ctx->cur_hdr_name_len,
                             &ctx->cur_hdr_name_cap, at, len);
}

static int _srv_on_header_value(llhttp_t* p, const char* at, size_t len) {
    _srv_parser_t* ctx = (_srv_parser_t*)p->data;
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
    ctx->cur_hdr_name_cap = 0;
    return 0;
}

static int _srv_on_body(llhttp_t* p, const char* at, size_t len) {
    _srv_parser_t* ctx = (_srv_parser_t*)p->data;
    return _body_append(&ctx->req.body, &ctx->req.body_len,
                        &ctx->req.body_cap, at, len);
}

static int _srv_on_headers_complete(llhttp_t* p) {
    _srv_parser_t* ctx = (_srv_parser_t*)p->data;
    const char* expect = http_header_find(
        ctx->req.headers, ctx->req.header_count, "Expect");
    if (expect && http_header_eq(expect, "100-continue") && ctx->transport) {
        const char* cont = "HTTP/1.1 100 Continue\r\n\r\n";
        ctx->transport->write(ctx->transport->conn, cont, 25);
    }
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
    sp->settings.on_headers_complete    = _srv_on_headers_complete;
    sp->settings.on_message_complete    = _srv_on_message_complete;
    llhttp_init(&sp->parser, HTTP_REQUEST, &sp->settings);
    sp->parser.data = sp;
}

static void _srv_parser_destroy(_srv_parser_t* sp) {
    free(sp->req.url);
    http_headers_free(sp->req.headers, sp->req.header_count);
    free(sp->req.body);
    free(sp->cur_hdr_name);
}

static void _srv_parser_reset(_srv_parser_t* sp) {
    _srv_parser_destroy(sp);
    memset(&sp->req, 0, sizeof(sp->req));
    sp->cur_hdr_name = NULL;
    sp->cur_hdr_name_len = 0;
    sp->cur_hdr_name_cap = 0;
    sp->url_cap = 0;
    sp->complete = false;
    llhttp_reset(&sp->parser);
}

void http_srv_conn_coroutine(void* arg) {
    http_srv_conn_ctx_t* ctx = (http_srv_conn_ctx_t*)arg;
    _srv_parser_t sp;
    _srv_parser_init(&sp);
    sp.transport = &ctx->transport;

    atomic_fetch_add(&ctx->srv->active_conns, 1);

    char* readbuf = (char*)malloc(HTTP_IO_BUF_SIZE);
    if (!readbuf) {
        atomic_fetch_sub(&ctx->srv->active_conns, 1);
        _transport_close(&ctx->transport);
        free(ctx);
        return;
    }
    bool keep_alive = true;

    while (keep_alive) {
        if (ctx->srv->closing) {
            break;
        }

        sp.complete = false;

        /* Idle timeout: wait for next request. */
        if (ctx->transport.set_rd_deadline) {
            uint64_t idle_deadline =
                xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                + ctx->srv->idle_timeout_ms;
            ctx->transport.set_rd_deadline(ctx->transport.conn, idle_deadline);
        }

        bool first_read = true;
        while (!sp.complete) {
            int n = _transport_read(&ctx->transport, readbuf, HTTP_IO_BUF_SIZE);
            if (n <= 0) {
                goto done; /* peer closed, timeout, or error */
            }

            /* Slowloris: tighten deadline after first data arrives. */
            if (first_read) {
                first_read = false;
                if (ctx->transport.set_rd_deadline) {
                    uint64_t hdr_deadline =
                        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                        + ctx->srv->read_header_timeout_ms;
                    ctx->transport.set_rd_deadline(
                        ctx->transport.conn, hdr_deadline);
                }
            }

            llhttp_errno_t err = llhttp_execute(&sp.parser, readbuf, (size_t)n);
            if (err == HPE_PAUSED) {
                llhttp_resume(&sp.parser);
            } else if (err == HPE_PAUSED_UPGRADE) {
                llhttp_resume_after_upgrade(&sp.parser);
                sp.complete = true;
            } else if (err != HPE_OK) {
                const char* resp;
                if (err == HPE_USER) {
                    resp = "HTTP/1.1 413 Payload Too Large\r\n"
                           "Content-Length: 0\r\nConnection: close\r\n\r\n";
                } else {
                    resp = "HTTP/1.1 400 Bad Request\r\n"
                           "Content-Length: 0\r\nConnection: close\r\n\r\n";
                }
                _transport_write(&ctx->transport, resp, (int)strlen(resp));
                goto done;
            }
        }

        /* Clear read deadline; set write deadline for response. */
        if (ctx->transport.set_rd_deadline) {
            ctx->transport.set_rd_deadline(ctx->transport.conn, 0);
        }
        if (ctx->transport.set_wr_deadline) {
            uint64_t wr_deadline =
                xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                + ctx->srv->write_timeout_ms;
            ctx->transport.set_wr_deadline(ctx->transport.conn, wr_deadline);
        }

        keep_alive = llhttp_should_keep_alive(&sp.parser) != 0;

        memcpy(sp.req.remote_host, ctx->remote_host, sizeof(ctx->remote_host));
        sp.req.remote_port = ctx->remote_port;

        xylem_http_res_t res;
        memset(&res, 0, sizeof(res));
        res._transport = &ctx->transport;
        res.status_code = 200;

        bool is_upgrade = llhttp_get_upgrade(&sp.parser) != 0;

        if (is_upgrade && ctx->srv->on_upgrade) {
            ctx->srv->on_upgrade(&res, &sp.req, ctx->srv->upgrade_userdata);
            http_headers_free(res.headers, res.header_count);
            _srv_parser_destroy(&sp);
            if (res._transport) {
                _transport_close(&ctx->transport);
            }
            atomic_fetch_sub(&ctx->srv->active_conns, 1);
            free(readbuf);
            free(ctx);
            return;
        }

        ctx->srv->handler(&res, &sp.req, ctx->srv->userdata);

        _finalize_response(&res);
        free(res._body_buf); /* safety: in case finalize did not handle it */

        http_headers_free(res.headers, res.header_count);

        _srv_parser_reset(&sp);
    }

done:
    atomic_fetch_sub(&ctx->srv->active_conns, 1);
    _srv_parser_destroy(&sp);
    free(readbuf);
    _transport_close(&ctx->transport);
    free(ctx);
}


typedef struct {
    llhttp_t          parser;
    llhttp_settings_t settings;
    xylem_http_res_t* res;
    char*             cur_hdr_name;
    size_t            cur_hdr_name_len;
    size_t            cur_hdr_name_cap;
    bool              complete;
} _cli_parser_t;

static int _cli_on_header_field(llhttp_t* p, const char* at, size_t len) {
    _cli_parser_t* ctx = (_cli_parser_t*)p->data;
    return _hdr_field_append(&ctx->cur_hdr_name, &ctx->cur_hdr_name_len,
                             &ctx->cur_hdr_name_cap, at, len);
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
    ctx->cur_hdr_name_cap = 0;
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
    return _body_append(&res->body, &res->body_len, &res->body_cap, at, len);
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

static int _resolve_redirect_url(const char* location, const http_url_t* base,
                                 http_url_t* out) {
    if (!location || !*location) {
        return -1;
    }

    if (strstr(location, "://") != NULL) {
        return http_url_parse(location, out);
    }

    *out = *base;

    if (location[0] == '/') {
        size_t loc_len = strlen(location);
        if (loc_len >= sizeof(out->path)) {
            return -1;
        }
        memcpy(out->path, location, loc_len + 1);
    } else {
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

static bool _http_acquire_conn(const http_url_t* parsed,
                               const xylem_http_cli_opts_t* opts,
                               http_dial_fn_t dial_fn,
                               void* dial_ctx,
                               http_transport_t* transport) {
    memset(transport, 0, sizeof(*transport));
    if (_pool_acquire(parsed, transport)) {
        return true;
    }

    uint64_t timeout = 10000;
    if (opts && opts->timeout_ms > 0) {
        timeout = opts->timeout_ms;
    }
    *transport = dial_fn(parsed->host, parsed->port, timeout, dial_ctx);
    return transport->conn != NULL;
}


static void _http_decompress_body(xylem_http_res_t* res) {
    const char* ce = http_header_find(res->headers, res->header_count,
                                      "Content-Encoding");
    if (!ce || !strstr(ce, "gzip") || !res->body || res->body_len == 0) {
        return;
    }

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

xylem_http_res_t* http_do_request(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts,
    bool                         use_proxy,
    http_dial_fn_t               dial_fn,
    void*                        dial_ctx) {

    if (!url || !dial_fn) {
        return NULL;
    }

    int redirects_left = 10;
    if (opts) {
        redirects_left = opts->max_redirects;
    }

    const char* cur_method = method;
    const void* cur_body = body;
    size_t cur_body_len = body_len;
    const char* cur_content_type = content_type;

    http_url_t parsed;
    if (http_url_parse(url, &parsed) != 0) {
        return NULL;
    }

    char* readbuf = (char*)malloc(HTTP_IO_BUF_SIZE);
    if (!readbuf) {
        return NULL;
    }

    for (;;) {

    http_transport_t transport;
    if (!_http_acquire_conn(&parsed, opts, dial_fn, dial_ctx, &transport)) {
        free(readbuf);
        return NULL;
    }

    uint64_t deadline_ms = 0;
    if (opts && opts->timeout_ms > 0) {
        deadline_ms = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                      + opts->timeout_ms;
    }
    if (transport.set_rd_deadline) {
        transport.set_rd_deadline(transport.conn, deadline_ms);
    }
    if (transport.set_wr_deadline) {
        transport.set_wr_deadline(transport.conn, deadline_ms);
    }

    const xylem_http_hdr_t* custom_hdrs = headers;
    size_t custom_hdr_count = header_count;

    bool use_expect = cur_body_len > 0;

    size_t req_len = 0;
    char* req_buf = http_req_serialize(
        cur_method, &parsed, cur_body, cur_body_len, cur_content_type,
        use_expect, use_proxy, &req_len, custom_hdrs, custom_hdr_count);

    if (!req_buf) {
        _transport_close(&transport);
        free(readbuf);
        return NULL;
    }

    int wrc = _transport_write(&transport, req_buf, (int)req_len);
    free(req_buf);
    if (wrc != 0) {
        _transport_close(&transport);
        free(readbuf);
        return NULL;
    }

    /* Expect/100-Continue: wait up to 1s for an interim response. */
    if (use_expect) {
        uint64_t expect_deadline =
            xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + 1000;
        if (transport.set_rd_deadline) {
            transport.set_rd_deadline(transport.conn, expect_deadline);
        }

        xylem_http_res_t* interim =
            (xylem_http_res_t*)calloc(1, sizeof(*interim));
        if (!interim) {
            _transport_close(&transport);
            free(readbuf);
            return NULL;
        }

        _cli_parser_t ecp;
        _cli_parser_init(&ecp, interim);

        bool got_response = false;
        while (!ecp.complete) {
            int n = _transport_read(&transport, readbuf, HTTP_IO_BUF_SIZE);
            if (n <= 0) {
                /* Timeout or error -- send body anyway per RFC 7231. */
                break;
            }
            llhttp_errno_t err =
                llhttp_execute(&ecp.parser, readbuf, (size_t)n);
            if (err == HPE_PAUSED) {
                llhttp_resume(&ecp.parser);
            } else if (err != HPE_OK) {
                break;
            }
        }
        got_response = ecp.complete;

        if (got_response && interim->status_code != 100) {
            /* Final error response (e.g. 413, 417) -- return it. */
            bool ka = llhttp_should_keep_alive(&ecp.parser) != 0;
            _cli_parser_destroy(&ecp);

            if (ka) {
                if (transport.set_rd_deadline) {
                    transport.set_rd_deadline(transport.conn, 0);
                }
                if (transport.set_wr_deadline) {
                    transport.set_wr_deadline(transport.conn, 0);
                }
                _pool_release(&parsed, &transport);
            } else {
                _transport_close(&transport);
            }

            free(readbuf);
            return interim;
        }

        /* 100-Continue received OR timeout -- send body per RFC 7231. */
        _cli_parser_destroy(&ecp);
        xylem_http_res_destroy(interim);

        if (transport.set_rd_deadline) {
            transport.set_rd_deadline(transport.conn, deadline_ms);
        }

        wrc = _transport_write(&transport, cur_body, (int)cur_body_len);
        if (wrc != 0) {
            _transport_close(&transport);
            free(readbuf);
            return NULL;
        }
    }

    xylem_http_res_t* res = (xylem_http_res_t*)calloc(1, sizeof(*res));
    if (!res) {
        _transport_close(&transport);
        free(readbuf);
        return NULL;
    }

    _cli_parser_t cp;
    _cli_parser_init(&cp, res);

    while (!cp.complete) {
        int n = _transport_read(&transport, readbuf, HTTP_IO_BUF_SIZE);
        if (n <= 0) {
            if (n == 0) {
                llhttp_finish(&cp.parser);
                if (cp.complete) {
                    break;
                }
            }
            _cli_parser_destroy(&cp);
            _transport_close(&transport);
            xylem_http_res_destroy(res);
            free(readbuf);
            return NULL;
        }

        llhttp_errno_t err = llhttp_execute(&cp.parser, readbuf, (size_t)n);
        if (err == HPE_PAUSED) {
            llhttp_resume(&cp.parser);
        } else if (err != HPE_OK) {
            _cli_parser_destroy(&cp);
            _transport_close(&transport);
            xylem_http_res_destroy(res);
            free(readbuf);
            return NULL;
        }
    }

    bool keep_alive = llhttp_should_keep_alive(&cp.parser) != 0;
    _cli_parser_destroy(&cp);

    if (keep_alive) {
        _pool_release(&parsed, &transport);
    } else {
        _transport_close(&transport);
    }

    int status = res->status_code;
    if (redirects_left > 0 &&
        (status == 301 || status == 302 || status == 303 ||
         status == 307 || status == 308)) {

        const char* location = http_header_find(res->headers, res->header_count,
                                                "Location");
        if (location) {
            http_url_t next_url;
            if (_resolve_redirect_url(location, &parsed, &next_url) == 0) {
                if (status != 307 && status != 308) {
                    cur_method = "GET";
                    cur_body = NULL;
                    cur_body_len = 0;
                    cur_content_type = NULL;
                }

                xylem_http_res_destroy(res);
                parsed = next_url;
                redirects_left--;
                continue;
            }
        }
    }

    _http_decompress_body(res);
    free(readbuf);
    return res;

    } /* for (;;) */
}


void xylem_http_serve_content(xylem_http_res_t* res,
                              xylem_http_req_t* req,
                              const void* data,
                              size_t data_len,
                              const char* content_type) {
    if (!res || !req || !data || data_len == 0 || !content_type) {
        if (res) {
            xylem_http_res_set_status(res, 500);
            xylem_http_res_write(res, "Internal Server Error", 21);
        }
        return;
    }

    const uint8_t* bytes = (const uint8_t*)data;

    /* Compute ETag: FNV-1a hash of content + size. */
    char etag[24];
    {
        uint32_t hash = 0x811c9dc5u;
        for (size_t i = 0; i < data_len; i++) {
            hash ^= bytes[i];
            hash *= 0x01000193u;
        }
        snprintf(etag, sizeof(etag), "\"%08x-%zx\"", hash, data_len);
    }

    /* If-None-Match: return 304. */
    const char* inm = xylem_http_req_header(req, "If-None-Match");
    if (inm && strcmp(inm, etag) == 0) {
        xylem_http_res_set_status(res, 304);
        xylem_http_res_set_header(res, "ETag", etag);
        return;
    }

    const char* method = xylem_http_req_method(req);

    /* Range request. */
    const char* range_hdr = xylem_http_req_header(req, "Range");
    if (range_hdr && strncmp(range_hdr, "bytes=", 6) == 0 &&
        strcmp(method, "HEAD") != 0) {
        size_t total = data_len;
        size_t start = 0, end_pos = total - 1;
        const char* spec = range_hdr + 6;

        if (spec[0] == '-') {
            size_t suffix = (size_t)strtoul(spec + 1, NULL, 10);
            if (suffix > total) {
                suffix = total;
            }
            start = total - suffix;
        } else {
            start = (size_t)strtoul(spec, NULL, 10);
            const char* dash = strchr(spec, '-');
            if (dash && dash[1] != '\0') {
                end_pos = (size_t)strtoul(dash + 1, NULL, 10);
            }
        }

        if (start > end_pos || start >= total) {
            xylem_http_res_set_status(res, 416);
            char cr[64];
            snprintf(cr, sizeof(cr), "bytes */%zu", total);
            xylem_http_res_set_header(res, "Content-Range", cr);
            xylem_http_res_write(res, "Range Not Satisfiable", 21);
            return;
        }
        if (end_pos >= total) {
            end_pos = total - 1;
        }

        char cr[64];
        snprintf(cr, sizeof(cr), "bytes %zu-%zu/%zu", start, end_pos, total);
        xylem_http_res_set_header(res, "Content-Range", cr);
        xylem_http_res_set_header(res, "Accept-Ranges", "bytes");
        xylem_http_res_set_header(res, "Content-Type", content_type);
        xylem_http_res_set_header(res, "ETag", etag);
        xylem_http_res_set_status(res, 206);
        xylem_http_res_write(res, bytes + start, end_pos - start + 1);
        return;
    }

    /* Normal 200 response. */
    xylem_http_res_set_header(res, "Accept-Ranges", "bytes");
    xylem_http_res_set_header(res, "Content-Type", content_type);
    xylem_http_res_set_header(res, "ETag", etag);
    xylem_http_res_set_status(res, 200);

    if (strcmp(method, "HEAD") != 0) {
        xylem_http_res_write(res, data, data_len);
    }
}


/* ===================================================================== *
 *  Server setup shared by the TCP/TLS transport factories
 * ===================================================================== */

void http_srv_init(http_srv_t* srv, const xylem_http_srv_opts_t* opts) {
    /* handler/userdata are set by the caller before this is invoked. */
    if (opts) {
        srv->on_upgrade       = opts->on_upgrade;
        srv->upgrade_userdata = opts->upgrade_userdata;
        srv->idle_timeout_ms  = opts->idle_timeout_ms;
        srv->read_header_timeout_ms = opts->read_header_timeout_ms;
        srv->write_timeout_ms = opts->write_timeout_ms;
    }
    if (!srv->idle_timeout_ms) {
        srv->idle_timeout_ms = 60000;
    }
    if (!srv->read_header_timeout_ms) {
        srv->read_header_timeout_ms = 10000;
    }
    if (!srv->write_timeout_ms) {
        srv->write_timeout_ms = 30000;
    }
}

/* ===================================================================== *
 *  Public API + scheme dispatch
 * ===================================================================== */

static bool _is_https(const char* url) {
    return url && url[0] == 'h' && url[1] == 't' && url[2] == 't'
        && url[3] == 'p' && url[4] == 's' && url[5] == ':';
}

xylem_http_srv_t* xylem_http_listen(
    const char*                  host,
    uint16_t                     port,
    xylem_http_handler_fn_t      handler,
    void*                        userdata,
    const xylem_http_srv_opts_t* opts) {
    if (!handler && !(opts && opts->on_upgrade)) {
        return NULL;
    }
    if (opts && opts->tls) {
        return http_tls_listen(host, port, handler, userdata, opts);
    }
    return http_tcp_listen(host, port, handler, userdata, opts);
}

void xylem_http_close(xylem_http_srv_t* srv) {
    if (!srv) {
        return;
    }
    http_srv_t* s = (http_srv_t*)srv;
    s->close_listener(s->listener);
    free(s);
}

int xylem_http_shutdown(xylem_http_srv_t* srv, uint64_t timeout_ms) {
    if (!srv) {
        return -1;
    }
    http_srv_t* s = (http_srv_t*)srv;
    s->closing = true;
    s->close_listener(s->listener);

    if (timeout_ms == 0) {
        free(s);
        return 0;
    }

    uint64_t deadline =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout_ms;
    while (atomic_load(&s->active_conns) > 0) {
        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        if (now >= deadline) {
            free(s);
            return -1;
        }
        runtime_sleep(1);
    }
    free(s);
    return 0;
}

int xylem_http_srv_addr(xylem_http_srv_t* srv,
                        char* host, size_t host_len,
                        uint16_t* port) {
    if (!srv) {
        return -1;
    }
    http_srv_t* s = (http_srv_t*)srv;
    if (host && host_len > 0) {
        snprintf(host, host_len, "%s", s->host);
    }
    if (port) {
        *port = s->port;
    }
    return 0;
}

static xylem_http_res_t* _do_request(const char* method, const char* url,
                                     const void* body, size_t body_len,
                                     const char* content_type,
                                     const xylem_http_hdr_t* headers,
                                     size_t header_count,
                                     const xylem_http_cli_opts_t* opts) {
    if (_is_https(url)) {
        return http_tls_request(method, url, body, body_len, content_type,
                                headers, header_count, opts);
    }
    return http_tcp_request(method, url, body, body_len, content_type,
                            headers, header_count, opts);
}

xylem_http_res_t* xylem_http_request(
    const char*                  method,
    const char*                  url,
    const void*                  body,
    size_t                       body_len,
    const char*                  content_type,
    const xylem_http_hdr_t*      headers,
    size_t                       header_count,
    const xylem_http_cli_opts_t* opts,
    xylem_http_cookie_jar_t*     jar) {
    (void)jar;
    return _do_request(method, url, body, body_len, content_type,
                       headers, header_count, opts);
}

xylem_http_res_t* xylem_http_get(const char* url,
                                 const xylem_http_hdr_t* headers,
                                 size_t header_count,
                                 const xylem_http_cli_opts_t* opts) {
    return _do_request("GET", url, NULL, 0, NULL, headers, header_count, opts);
}

xylem_http_res_t* xylem_http_head(const char* url,
                                  const xylem_http_hdr_t* headers,
                                  size_t header_count,
                                  const xylem_http_cli_opts_t* opts) {
    return _do_request("HEAD", url, NULL, 0, NULL, headers, header_count, opts);
}

xylem_http_res_t* xylem_http_post(const char* url,
                                  const void* body, size_t body_len,
                                  const char* content_type,
                                  const xylem_http_hdr_t* headers,
                                  size_t header_count,
                                  const xylem_http_cli_opts_t* opts) {
    return _do_request("POST", url, body, body_len, content_type,
                       headers, header_count, opts);
}

xylem_http_res_t* xylem_http_put(const char* url,
                                 const void* body, size_t body_len,
                                 const char* content_type,
                                 const xylem_http_hdr_t* headers,
                                 size_t header_count,
                                 const xylem_http_cli_opts_t* opts) {
    return _do_request("PUT", url, body, body_len, content_type,
                       headers, header_count, opts);
}

xylem_http_res_t* xylem_http_delete(const char* url,
                                    const xylem_http_hdr_t* headers,
                                    size_t header_count,
                                    const xylem_http_cli_opts_t* opts) {
    return _do_request("DELETE", url, NULL, 0, NULL,
                       headers, header_count, opts);
}

xylem_http_res_t* xylem_http_patch(const char* url,
                                   const void* body, size_t body_len,
                                   const char* content_type,
                                   const xylem_http_hdr_t* headers,
                                   size_t header_count,
                                   const xylem_http_cli_opts_t* opts) {
    return _do_request("PATCH", url, body, body_len, content_type,
                       headers, header_count, opts);
}
