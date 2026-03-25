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

#include "xylem/http/xylem-http-server.h"
#include "xylem/xylem-addr.h"
#include "xylem/xylem-loop.h"

#include "xylem/xylem-gzip.h"

#include "miniz.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform/platform-info.h"
#include "platform/platform-io.h"

struct xylem_http_req_s {
    char           method[16];
    char*          url;
    size_t         url_len;
    http_header_t* headers;
    size_t         header_count;
    size_t         header_cap;
    uint8_t*       body;
    size_t         body_len;
    http_header_t* params;
    size_t         param_count;
};

struct xylem_http_conn_s {
    xylem_http_srv_t*          srv;
    const http_transport_vt_t* vt;
    void*                      transport;
    llhttp_t                   parser;
    llhttp_settings_t          settings;
    xylem_http_req_t           req;
    xylem_loop_timer_t         idle_timer;
    bool                       keep_alive;
    bool                       closed;
    bool                       chunked_active;
    char*                      cur_header_name;
    size_t                     cur_header_name_len;
    bool                       expect_continue;
    /* Writer mode state */
    xylem_http_hdr_t*          resp_headers;
    size_t                     resp_header_count;
    size_t                     resp_header_cap;
    int                        resp_status;
    bool                       resp_headers_sent;
    /* Streaming gzip state */
    mz_stream*                 gzip_stream;
    bool                       gzip_active;
    bool                       cl_mode;
    /* Upgrade state */
    bool                       in_upgrade_cb;
    bool                       upgrade_accepted;
};

struct xylem_http_srv_s {
    xylem_loop_t*              loop;
    xylem_http_srv_cfg_t       cfg;
    const http_transport_vt_t* vt;
    void*                      listener;
    http_transport_cb_t        transport_cb;
    bool                       running;
    xylem_http_gzip_opts_t     gzip_opts;
};

static const char* _http_day_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char* _http_month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* Default MIME types eligible for gzip compression. */
static const char* _gzip_default_types[] = {
    "text/html",
    "text/css",
    "text/plain",
    "text/xml",
    "text/javascript",
    "application/json",
    "application/javascript",
    "application/xml",
    "image/svg+xml",
    NULL
};

static const struct {
    const char* ext;
    const char* mime;
} _mime_map[] = {
    {".html", "text/html"},
    {".htm",  "text/html"},
    {".css",  "text/css"},
    {".js",   "application/javascript"},
    {".json", "application/json"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".svg",  "image/svg+xml"},
    {".ico",  "image/x-icon"},
    {".woff2","font/woff2"},
    {".woff", "font/woff"},
    {".ttf",  "font/ttf"},
    {".txt",  "text/plain"},
    {".xml",  "application/xml"},
    {".pdf",  "application/pdf"},
    {".wasm", "application/wasm"},
    {".mp4",  "video/mp4"},
    {".webp", "image/webp"},
    {".webm", "video/webm"},
    {".map",  "application/json"},
    {NULL, NULL}
};

/* Forward declaration — defined later, needed by flush_resp_headers. */
static size_t _http_format_date(time_t t, char* buf, size_t cap);

static void _http_srv_req_reset(xylem_http_req_t* req) {
    free(req->url);
    http_headers_free(req->headers, req->header_count);
    free(req->body);
    http_headers_free(req->params, req->param_count);
    memset(req, 0, sizeof(*req));
}

static void _http_srv_conn_destroy(xylem_http_conn_t* conn) {
    if (!conn) {
        return;
    }
    _http_srv_req_reset(&conn->req);
    free(conn->cur_header_name);
    if (conn->idle_timer.active) {
        xylem_loop_stop_timer(&conn->idle_timer);
    }
    xylem_loop_close_timer(&conn->idle_timer);
    for (size_t i = 0; i < conn->resp_header_count; i++) {
        free((char*)conn->resp_headers[i].name);
        free((char*)conn->resp_headers[i].value);
    }
    free(conn->resp_headers);
    if (conn->gzip_stream) {
        mz_deflateEnd(conn->gzip_stream);
        free(conn->gzip_stream);
    }
    free(conn);
}

static int _http_srv_parser_url_cb(llhttp_t* parser, const char* at,
                                   size_t len) {
    xylem_http_conn_t* conn = parser->data;
    xylem_http_req_t* req = &conn->req;

    /* Accumulate URL fragments using tracked offset. */
    char* tmp = realloc(req->url, req->url_len + len + 1);
    if (!tmp) {
        return HPE_USER;
    }
    memcpy(tmp + req->url_len, at, len);
    req->url_len += len;
    tmp[req->url_len] = '\0';
    req->url = tmp;
    return 0;
}

static int _http_srv_parser_method_complete_cb(llhttp_t* parser) {
    xylem_http_conn_t* conn = parser->data;
    const char* m = llhttp_method_name(llhttp_get_method(parser));
    snprintf(conn->req.method, sizeof(conn->req.method), "%s", m);
    return 0;
}

static int _http_srv_parser_header_field_cb(llhttp_t* parser,
                                            const char* at, size_t len) {
    xylem_http_conn_t* conn = parser->data;

    free(conn->cur_header_name);
    conn->cur_header_name = malloc(len + 1);
    if (!conn->cur_header_name) {
        return HPE_USER;
    }
    memcpy(conn->cur_header_name, at, len);
    conn->cur_header_name[len] = '\0';
    conn->cur_header_name_len = len;
    return 0;
}

static int _http_srv_parser_header_value_cb(llhttp_t* parser,
                                            const char* at, size_t len) {
    xylem_http_conn_t* conn = parser->data;
    if (!conn->cur_header_name) {
        return 0;
    }

    if (http_header_add(&conn->req.headers, &conn->req.header_count,
                        &conn->req.header_cap,
                        conn->cur_header_name, conn->cur_header_name_len,
                        at, len) != 0) {
        return HPE_USER;
    }

    /* Check for Expect: 100-continue. */
    if (conn->cur_header_name_len == 6 &&
        http_header_eq(conn->cur_header_name, "Expect")) {
        char val[32];
        size_t copy_len = (len < sizeof(val) - 1) ? len : sizeof(val) - 1;
        memcpy(val, at, copy_len);
        val[copy_len] = '\0';
        if (http_header_eq(val, "100-continue")) {
            conn->expect_continue = true;
        }
    }

    free(conn->cur_header_name);
    conn->cur_header_name = NULL;
    conn->cur_header_name_len = 0;
    return 0;
}

static int _http_srv_parser_headers_complete_cb(llhttp_t* parser) {
    xylem_http_conn_t* conn = parser->data;

    conn->keep_alive = llhttp_should_keep_alive(parser);

    /* Send 100 Continue if the client expects it. */
    if (conn->expect_continue && !conn->closed) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        conn->vt->send(conn->transport, cont, sizeof(cont) - 1);
        conn->expect_continue = false;
    }

    /* Pre-allocate body buffer if Content-Length is known. */
    uint64_t content_length = parser->content_length;
    if (content_length > 0 && content_length != ULLONG_MAX &&
        content_length <= conn->srv->cfg.max_body_size) {
        conn->req.body = malloc((size_t)content_length);
        /* Allocation failure is non-fatal; realloc path handles it. */
    }

    return 0;
}

static int _http_srv_parser_body_cb(llhttp_t* parser, const char* at,
                                    size_t len) {
    xylem_http_conn_t* conn = parser->data;
    xylem_http_req_t* req = &conn->req;

    /* Enforce max body size. */
    if (req->body_len + len > conn->srv->cfg.max_body_size) {
        return HPE_USER;
    }

    /* If body was pre-allocated from Content-Length, just memcpy. */
    uint64_t content_length = parser->content_length;
    if (req->body && content_length != ULLONG_MAX &&
        req->body_len + len <= (size_t)content_length) {
        memcpy(req->body + req->body_len, at, len);
        req->body_len += len;
        return 0;
    }

    uint8_t* tmp = realloc(req->body, req->body_len + len);
    if (!tmp) {
        return HPE_USER;
    }
    memcpy(tmp + req->body_len, at, len);
    req->body = tmp;
    req->body_len += len;
    return 0;
}

/* Forward declaration --_http_srv_send is defined below write(). */
static int _http_srv_send(xylem_http_conn_t* conn,
                          int status_code,
                          const char* content_type,
                          const void* body, size_t body_len,
                          const xylem_http_hdr_t* headers,
                          size_t header_count);

static int _http_srv_parser_message_complete_cb(llhttp_t* parser) {
    xylem_http_conn_t* conn = parser->data;

    /* Reset idle timer --a complete request was received. */
    if (conn->idle_timer.active && conn->srv->cfg.idle_timeout_ms > 0) {
        xylem_loop_reset_timer(&conn->idle_timer,
                               conn->srv->cfg.idle_timeout_ms);
    }

    /* Check for Upgrade request (Connection: Upgrade flag set by llhttp). */
    bool is_upgrade = (parser->flags & F_CONNECTION_UPGRADE) != 0;

    if (is_upgrade) {
        if (conn->srv->cfg.on_upgrade) {
            conn->in_upgrade_cb = true;
            conn->srv->cfg.on_upgrade(conn, &conn->req,
                                      conn->srv->cfg.userdata);
            conn->in_upgrade_cb = false;

            /* User did not accept — close the connection. */
            if (!conn->upgrade_accepted && !conn->closed) {
                conn->closed = true;
                conn->vt->close_conn(conn->transport);
            }
        } else {
            /* No on_upgrade callback: reject with 501. */
            _http_srv_send(conn, 501, "text/plain",
                           "Not Implemented", 15, NULL, 0);
            conn->closed = true;
            conn->vt->close_conn(conn->transport);
        }
    } else {
        /* Normal request dispatch. */
        if (conn->srv->cfg.on_request) {
            conn->srv->cfg.on_request(conn, &conn->req,
                                      conn->srv->cfg.userdata);
        }
    }

    return HPE_PAUSED;
}

static void _http_srv_idle_timeout_cb(xylem_loop_t* loop,
                                      xylem_loop_timer_t* timer) {
    (void)loop;
    xylem_http_conn_t* conn =
        (xylem_http_conn_t*)((char*)timer -
            offsetof(xylem_http_conn_t, idle_timer));
    if (!conn->closed) {
        conn->closed = true;
        conn->vt->close_conn(conn->transport);
    }
}

static void _http_srv_conn_init_parser(xylem_http_conn_t* conn) {
    llhttp_settings_init(&conn->settings);
    conn->settings.on_url              = _http_srv_parser_url_cb;
    conn->settings.on_method_complete  = _http_srv_parser_method_complete_cb;
    conn->settings.on_header_field     = _http_srv_parser_header_field_cb;
    conn->settings.on_header_value     = _http_srv_parser_header_value_cb;
    conn->settings.on_headers_complete = _http_srv_parser_headers_complete_cb;
    conn->settings.on_body             = _http_srv_parser_body_cb;
    conn->settings.on_message_complete = _http_srv_parser_message_complete_cb;
    llhttp_init(&conn->parser, HTTP_REQUEST, &conn->settings);
    conn->parser.data = conn;
}

/**
 * After a response is fully sent, either prepare for the next
 * request (keep-alive) or close the connection.
 */
static void _http_srv_conn_finish_response(xylem_http_conn_t* conn) {
    if (conn->keep_alive && !conn->closed) {
        llhttp_resume(&conn->parser);
        _http_srv_conn_init_parser(conn);

        if (conn->srv->cfg.idle_timeout_ms > 0) {
            if (conn->idle_timer.active) {
                xylem_loop_reset_timer(&conn->idle_timer,
                                       conn->srv->cfg.idle_timeout_ms);
            } else {
                xylem_loop_start_timer(&conn->idle_timer,
                                       _http_srv_idle_timeout_cb,
                                       conn->srv->cfg.idle_timeout_ms, 0);
            }
        }
    } else if (!conn->closed) {
        conn->closed = true;
        conn->vt->close_conn(conn->transport);
    }
}

static void _http_srv_conn_accept_cb(void* handle, void* ctx) {
    xylem_http_srv_t* srv = ctx;

    xylem_http_conn_t* conn = calloc(1, sizeof(*conn));
    if (!conn) {
        srv->vt->close_conn(handle);
        return;
    }

    conn->srv       = srv;
    conn->vt        = srv->vt;
    conn->transport = handle;
    conn->resp_status = 200;

    _http_srv_conn_init_parser(conn);

    /* Start idle timer. */
    xylem_loop_init_timer(srv->loop, &conn->idle_timer);
    if (srv->cfg.idle_timeout_ms > 0) {
        xylem_loop_start_timer(&conn->idle_timer,
                               _http_srv_idle_timeout_cb,
                               srv->cfg.idle_timeout_ms, 0);
    }

    /* Store conn pointer as userdata on the transport handle. */
    srv->vt->set_userdata(handle, conn);
}

/* Free all buffered response headers and reset writer state. */
static void _http_srv_resp_reset(xylem_http_conn_t* conn) {
    for (size_t i = 0; i < conn->resp_header_count; i++) {
        free((char*)conn->resp_headers[i].name);
        free((char*)conn->resp_headers[i].value);
    }
    free(conn->resp_headers);
    conn->resp_headers = NULL;
    conn->resp_header_count = 0;
    conn->resp_header_cap = 0;
    conn->resp_status = 200;
    conn->resp_headers_sent = false;

    /* Clean up streaming gzip state. */
    if (conn->gzip_stream) {
        mz_deflateEnd(conn->gzip_stream);
        free(conn->gzip_stream);
        conn->gzip_stream = NULL;
    }
    conn->gzip_active = false;
    conn->cl_mode = false;
    conn->in_upgrade_cb = false;
    conn->upgrade_accepted = false;
}

static void _http_srv_conn_read_cb(void* handle, void* ctx,
                                   void* data, size_t len) {
    xylem_http_srv_t* srv = ctx;

    xylem_http_conn_t* conn = srv->vt->get_userdata(handle);
    if (!conn || conn->closed) {
        return;
    }

    const char* p = (const char*)data;
    size_t remaining = len;

    while (remaining > 0 && !conn->closed) {
        enum llhttp_errno err = llhttp_execute(&conn->parser, p, remaining);

        if (err == HPE_PAUSED) {
            /* Calculate consumed bytes before the pause point. */
            size_t consumed =
                (size_t)(llhttp_get_error_pos(&conn->parser) - p);
            p += consumed;
            remaining -= consumed;

            /**
             * Message complete — the on_request callback has fired.
             * If upgrade was accepted, transport ownership transferred
             * to user — skip auto-finish and destroy the conn.
             */
            if (conn->upgrade_accepted) {
                _http_srv_req_reset(&conn->req);
                free(conn->cur_header_name);
                conn->cur_header_name = NULL;
                conn->cur_header_name_len = 0;
                _http_srv_resp_reset(conn);
                _http_srv_conn_destroy(conn);
                return;
            }

            /**
             * Auto-finish: finalize gzip stream if active, then send
             * the terminating chunk (unless in CL mode).
             */
            if (conn->gzip_active && !conn->closed) {
                uint8_t finish_buf[256];
                conn->gzip_stream->next_in = NULL;
                conn->gzip_stream->avail_in = 0;
                conn->gzip_stream->next_out = finish_buf;
                conn->gzip_stream->avail_out = sizeof(finish_buf);

                int zrc = mz_deflate(conn->gzip_stream, MZ_FINISH);
                size_t produced =
                    sizeof(finish_buf) - conn->gzip_stream->avail_out;
                if (produced > 0 && (zrc == MZ_OK || zrc == MZ_STREAM_END)) {
                    _http_srv_send_chunk(conn, finish_buf, produced);
                }
                while (zrc == MZ_OK) {
                    conn->gzip_stream->next_out = finish_buf;
                    conn->gzip_stream->avail_out = sizeof(finish_buf);
                    zrc = mz_deflate(conn->gzip_stream, MZ_FINISH);
                    produced =
                        sizeof(finish_buf) - conn->gzip_stream->avail_out;
                    if (produced > 0) {
                        _http_srv_send_chunk(conn, finish_buf, produced);
                    }
                }
                conn->gzip_active = false;
            }

            if (conn->chunked_active && !conn->closed) {
                static const char terminator[] = "0\r\n\r\n";
                conn->vt->send(conn->transport, terminator,
                               sizeof(terminator) - 1);
                conn->chunked_active = false;
            }

            _http_srv_req_reset(&conn->req);
            free(conn->cur_header_name);
            conn->cur_header_name = NULL;
            conn->cur_header_name_len = 0;
            conn->expect_continue = false;
            _http_srv_resp_reset(conn);

            /* Resume parser for next request on keep-alive. */
            _http_srv_conn_finish_response(conn);

            if (conn->closed) {
                break;
            }
            /* Loop continues to parse remaining pipelined data. */
            continue;
        }

        if (err == HPE_USER) {
            /* Body too large — send 413 and close. */
            if (!conn->closed) {
                static const char resp_413[] =
                    "HTTP/1.1 413 Payload Too Large\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n"
                    "\r\n";
                conn->vt->send(conn->transport, resp_413,
                               sizeof(resp_413) - 1);
                conn->closed = true;
                conn->vt->close_conn(conn->transport);
            }
            return;
        }

        if (err != HPE_OK) {
            /* Parse error — close connection. */
            if (!conn->closed) {
                conn->closed = true;
                conn->vt->close_conn(conn->transport);
            }
            return;
        }

        /* HPE_OK: all data consumed, need more. */
        break;
    }
}

static void _http_srv_conn_close_cb(void* handle, void* ctx, int err) {
    (void)err;
    xylem_http_srv_t* srv = ctx;
    xylem_http_conn_t* conn = srv->vt->get_userdata(handle);
    if (conn) {
        _http_srv_conn_destroy(conn);
    }
}

const char* xylem_http_req_method(const xylem_http_req_t* req) {
    if (!req) {
        return NULL;
    }
    return req->method;
}

const char* xylem_http_req_url(const xylem_http_req_t* req) {
    if (!req) {
        return NULL;
    }
    return req->url;
}

const char* xylem_http_req_header(const xylem_http_req_t* req,
                                  const char* name) {
    if (!req || !name) {
        return NULL;
    }
    return http_header_find(req->headers, req->header_count, name);
}

const void* xylem_http_req_body(const xylem_http_req_t* req) {
    if (!req) {
        return NULL;
    }
    return req->body;
}

size_t xylem_http_req_body_len(const xylem_http_req_t* req) {
    if (!req) {
        return 0;
    }
    return req->body_len;
}

xylem_http_srv_t* xylem_http_listen(xylem_loop_t* loop,
                                    const xylem_http_srv_cfg_t* cfg) {
    if (!loop || !cfg) {
        return NULL;
    }

    xylem_http_srv_t* srv = calloc(1, sizeof(*srv));
    if (!srv) {
        return NULL;
    }

    srv->loop = loop;
    srv->cfg  = *cfg;

    if (srv->cfg.max_body_size == 0) {
        srv->cfg.max_body_size = 1048576; /* 1 MiB */
    }
    if (srv->cfg.idle_timeout_ms == 0) {
        srv->cfg.idle_timeout_ms = 60000;
    }

    if (cfg->tls_cert && cfg->tls_key) {
        srv->vt = http_transport_tls();
        if (!srv->vt) {
            free(srv);
            return NULL;
        }
    } else {
        srv->vt = http_transport_tcp();
    }

    /* Bind and start listening. */
    srv->transport_cb.on_accept     = _http_srv_conn_accept_cb;
    srv->transport_cb.on_read       = _http_srv_conn_read_cb;
    srv->transport_cb.on_close      = _http_srv_conn_close_cb;
    srv->transport_cb.on_connect    = NULL;
    srv->transport_cb.on_write_done = NULL;

    const char* host = srv->cfg.host ? srv->cfg.host : "0.0.0.0";
    xylem_addr_t addr;
    if (xylem_addr_pton(host, srv->cfg.port, &addr) != 0) {
        free(srv);
        return NULL;
    }

    srv->listener = srv->vt->listen(srv->loop, &addr,
                                    &srv->transport_cb, srv, NULL,
                                    srv->cfg.tls_cert, srv->cfg.tls_key);
    if (!srv->listener) {
        free(srv);
        return NULL;
    }

    srv->running = true;
    return srv;
}

void xylem_http_close_server(xylem_http_srv_t* srv) {
    if (!srv) {
        return;
    }
    if (srv->running) {
        srv->vt->close_server(srv->listener);
        srv->listener = NULL;
        srv->running = false;
    }
    free(srv);
}

void xylem_http_srv_set_gzip(xylem_http_srv_t* srv,
                             const xylem_http_gzip_opts_t* opts) {
    if (!srv || !opts) {
        return;
    }
    srv->gzip_opts = *opts;
    if (srv->gzip_opts.level == 0) {
        srv->gzip_opts.level = 6;
    }
    if (srv->gzip_opts.min_size == 0) {
        srv->gzip_opts.min_size = 1024;
    }
}

/* Check if content_type matches a MIME pattern (prefix match for "text/*"). */
static bool _http_gzip_mime_match(const char* content_type, const char* pat) {
    size_t plen = strlen(pat);
    /* Wildcard: "text/*" matches any "text/..." */
    if (plen >= 2 && pat[plen - 1] == '*' && pat[plen - 2] == '/') {
        return strncmp(content_type, pat, plen - 1) == 0;
    }
    /* Exact prefix match up to ';' (ignore charset etc.) */
    size_t ctlen = strlen(content_type);
    const char* semi = memchr(content_type, ';', ctlen);
    size_t cmplen = semi ? (size_t)(semi - content_type) : ctlen;
    /* Trim trailing spaces before semicolon. */
    while (cmplen > 0 && content_type[cmplen - 1] == ' ') {
        cmplen--;
    }
    return cmplen == plen && strncmp(content_type, pat, plen) == 0;
}

/* Check if content_type is compressible per the gzip options. */
static bool _http_gzip_type_ok(const xylem_http_gzip_opts_t* opts,
                               const char* content_type) {
    if (!content_type) {
        return false;
    }
    if (!opts->mime_types) {
        /* Use built-in defaults. */
        for (size_t i = 0; _gzip_default_types[i]; i++) {
            if (_http_gzip_mime_match(content_type, _gzip_default_types[i])) {
                return true;
            }
        }
        return false;
    }
    /* Parse comma-separated user list. */
    const char* p = opts->mime_types;
    while (*p) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        const char* start = p;
        while (*p && *p != ',') {
            p++;
        }
        size_t len = (size_t)(p - start);
        /* Trim trailing spaces. */
        while (len > 0 && start[len - 1] == ' ') {
            len--;
        }
        if (len > 0) {
            char tmp[128];
            if (len < sizeof(tmp)) {
                memcpy(tmp, start, len);
                tmp[len] = '\0';
                if (_http_gzip_mime_match(content_type, tmp)) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* Check if Accept-Encoding header contains "gzip". */
static bool _http_gzip_accepted(const xylem_http_req_t* req) {
    const char* ae = http_header_find(req->headers, req->header_count,
                                      "Accept-Encoding");
    if (!ae) {
        return false;
    }
    /* Simple substring search for "gzip" with quality check. */
    const char* p = ae;
    while ((p = strstr(p, "gzip")) != NULL) {
        /* Verify it's a token boundary (not part of a longer word). */
        if (p != ae && *(p - 1) != ' ' && *(p - 1) != ',') {
            p += 4;
            continue;
        }
        const char* after = p + 4;
        if (*after == '\0' || *after == ',' || *after == ' ') {
            return true;
        }
        /* Check for q=0 (explicitly rejected). */
        if (*after == ';') {
            after++;
            while (*after == ' ') {
                after++;
            }
            if (*after == 'q' && *(after + 1) == '=') {
                after += 2;
                if (*after == '0' && (*(after + 1) == '\0' ||
                    *(after + 1) == ',' || *(after + 1) == ' ' ||
                    (*(after + 1) == '.' && *(after + 2) == '0'))) {
                    return false;
                }
            }
            return true;
        }
        p += 4;
    }
    return false;
}

/**
 * Decide whether to compress and, if so, perform gzip compression.
 * Returns compressed data in *out_buf (caller frees) and size in *out_len.
 * Returns true if compression was applied, false otherwise.
 */
static bool _http_srv_try_gzip(const xylem_http_srv_t* srv,
                               const xylem_http_req_t* req,
                               const char* content_type,
                               const void* body, size_t body_len,
                               const xylem_http_hdr_t* headers,
                               size_t header_count,
                               uint8_t** out_buf, size_t* out_len) {
    if (!srv->gzip_opts.enabled) {
        return false;
    }
    if (body_len < srv->gzip_opts.min_size) {
        return false;
    }
    /* Skip if Content-Encoding already set by custom headers. */
    for (size_t i = 0; i < header_count; i++) {
        if (headers[i].name &&
            http_header_eq(headers[i].name, "Content-Encoding")) {
            return false;
        }
    }
    if (!_http_gzip_type_ok(&srv->gzip_opts, content_type)) {
        return false;
    }
    if (!_http_gzip_accepted(req)) {
        return false;
    }

    size_t bound = xylem_gzip_compress_bound(body_len);
    uint8_t* cbuf = malloc(bound);
    if (!cbuf) {
        return false;
    }
    int clen = xylem_gzip_compress((const uint8_t*)body, body_len,
                                   cbuf, bound, srv->gzip_opts.level);
    if (clen < 0 || (size_t)clen >= body_len) {
        /* Compression failed or didn't shrink --skip. */
        free(cbuf);
        return false;
    }
    *out_buf = cbuf;
    *out_len = (size_t)clen;
    return true;
}



/* Add or replace a header in the conn's response header buffer. */
static int _http_srv_resp_header_set(xylem_http_conn_t* conn,
                                     const char* name,
                                     const char* value) {
    /* Check for existing header with same name (case-insensitive). */
    for (size_t i = 0; i < conn->resp_header_count; i++) {
        if (http_header_eq(conn->resp_headers[i].name, name)) {
            char* dup = strdup(value);
            if (!dup) {
                return -1;
            }
            free((char*)conn->resp_headers[i].value);
            conn->resp_headers[i].value = dup;
            return 0;
        }
    }

    /* Grow buffer if needed. */
    if (conn->resp_header_count == conn->resp_header_cap) {
        size_t new_cap = conn->resp_header_cap ? conn->resp_header_cap * 2 : 8;
        xylem_http_hdr_t* tmp = realloc(conn->resp_headers,
                                        new_cap * sizeof(*tmp));
        if (!tmp) {
            return -1;
        }
        conn->resp_headers = tmp;
        conn->resp_header_cap = new_cap;
    }

    char* dup_name = strdup(name);
    char* dup_value = strdup(value);
    if (!dup_name || !dup_value) {
        free(dup_name);
        free(dup_value);
        return -1;
    }

    conn->resp_headers[conn->resp_header_count].name = dup_name;
    conn->resp_headers[conn->resp_header_count].value = dup_value;
    conn->resp_header_count++;
    return 0;
}

/* Check if a header name exists in the conn's response header buffer. */
static bool _http_srv_resp_header_has(xylem_http_conn_t* conn,
                                      const char* name) {
    for (size_t i = 0; i < conn->resp_header_count; i++) {
        if (http_header_eq(conn->resp_headers[i].name, name)) {
            return true;
        }
    }
    return false;
}

/* Get a response header value from the conn's buffer, or NULL. */
static const char* _http_srv_resp_header_get(xylem_http_conn_t* conn,
                                             const char* name) {
    for (size_t i = 0; i < conn->resp_header_count; i++) {
        if (http_header_eq(conn->resp_headers[i].name, name)) {
            return conn->resp_headers[i].value;
        }
    }
    return NULL;
}

/**
 * Ensure a field name is present in the Vary header.
 *
 * If no Vary header exists, creates one with the given field.
 * If Vary already exists, parses its comma-separated values and
 * appends the field only if not already present (case-insensitive).
 * If Vary is "*", does nothing.
 */
static void _http_srv_vary_ensure(xylem_http_conn_t* conn,
                                  const char* field) {
    const char* existing = _http_srv_resp_header_get(conn, "Vary");

    if (!existing) {
        _http_srv_resp_header_set(conn, "Vary", field);
        return;
    }

    /* Vary: * means "varies on everything", no need to enumerate. */
    if (existing[0] == '*' && (existing[1] == '\0' || existing[1] == ',')) {
        return;
    }

    /* Check if field already present (case-insensitive token scan). */
    size_t field_len = strlen(field);
    const char* p = existing;
    while (*p) {
        while (*p == ',' || *p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char* start = p;
        while (*p && *p != ',' && *p != ' ') {
            p++;
        }
        size_t token_len = (size_t)(p - start);
        if (token_len == field_len) {
            bool eq = true;
            for (size_t i = 0; i < token_len; i++) {
                if (http_lower_table[(uint8_t)start[i]] !=
                    http_lower_table[(uint8_t)field[i]]) {
                    eq = false;
                    break;
                }
            }
            if (eq) {
                return; /* Already present. */
            }
        }
    }

    /* Append: "existing, field" */
    size_t elen = strlen(existing);
    size_t flen = field_len;
    char* merged = malloc(elen + 2 + flen + 1);
    if (!merged) {
        return; /* Silent fail — Vary missing won't break correctness. */
    }
    memcpy(merged, existing, elen);
    merged[elen] = ',';
    merged[elen + 1] = ' ';
    memcpy(merged + elen + 2, field, flen);
    merged[elen + 2 + flen] = '\0';

    _http_srv_resp_header_set(conn, "Vary", merged);
    free(merged);
}

/**
 * Decide whether streaming gzip should be activated for write() mode.
 * Checks: server gzip enabled, no user Content-Encoding, no user
 * Content-Length (CL mode skips gzip), Content-Type matches, and
 * client Accept-Encoding contains gzip.
 */
static bool _http_srv_should_gzip(xylem_http_conn_t* conn) {
    if (!conn->srv->gzip_opts.enabled) {
        return false;
    }
    /* User explicitly set Content-Encoding --respect it. */
    if (_http_srv_resp_header_has(conn, "Content-Encoding")) {
        return false;
    }
    /* Content-Length mode means the user knows the exact size;
       streaming gzip would change the size, so skip. */
    if (_http_srv_resp_header_has(conn, "Content-Length")) {
        return false;
    }
    /* Check Content-Type against compressible MIME types. */
    const char* ct = _http_srv_resp_header_get(conn, "Content-Type");
    if (!_http_gzip_type_ok(&conn->srv->gzip_opts, ct)) {
        return false;
    }
    /* Check client Accept-Encoding. */
    if (!_http_gzip_accepted(&conn->req)) {
        return false;
    }
    return true;
}

/*
 * Flush buffered response headers with Content-Length (non-chunked).
 * Sends: status line + buffered headers + Content-Length + CRLF.
 * Does NOT send the body --caller appends it after this call.
 * Returns bytes written into *out_buf (caller frees), or -1 on error.
 */
static int _http_srv_flush_resp_headers_cl(xylem_http_conn_t* conn,
                                           size_t content_length,
                                           char** out_buf,
                                           size_t* out_len) {
    /* Auto-add Date header if not already set (RFC 9110 §6.6.1). */
    if (!_http_srv_resp_header_has(conn, "Date")) {
        char date_buf[32];
        time_t now = time(NULL);
        _http_format_date(now, date_buf, sizeof(date_buf));
        _http_srv_resp_header_set(conn, "Date", date_buf);
    }

    /* Auto-add Connection: close when not keeping alive (RFC 9112 §9.6). */
    if (!conn->keep_alive &&
        !_http_srv_resp_header_has(conn, "Connection")) {
        _http_srv_resp_header_set(conn, "Connection", "close");
    }

    size_t est = 40 + 16 + 20 + 2 + 2; /* status line + CL header + CRLF */
    for (size_t i = 0; i < conn->resp_header_count; i++) {
        if (conn->resp_headers[i].name && conn->resp_headers[i].value) {
            est += strlen(conn->resp_headers[i].name)
                 + strlen(conn->resp_headers[i].value) + 4;
        }
    }

    char* buf = malloc(est);
    if (!buf) {
        return -1;
    }

    const char* reason = http_reason_phrase(conn->resp_status);
    size_t reason_len = strlen(reason);
    size_t off = 0;

    memcpy(buf + off, "HTTP/1.1 ", 9);
    off += 9;
    buf[off++] = (char)('0' + conn->resp_status / 100);
    buf[off++] = (char)('0' + (conn->resp_status / 10) % 10);
    buf[off++] = (char)('0' + conn->resp_status % 10);
    buf[off++] = ' ';
    memcpy(buf + off, reason, reason_len);
    off += reason_len;
    buf[off++] = '\r';
    buf[off++] = '\n';

    for (size_t i = 0; i < conn->resp_header_count; i++) {
        if (!conn->resp_headers[i].name || !conn->resp_headers[i].value) {
            continue;
        }
        size_t nlen = strlen(conn->resp_headers[i].name);
        size_t vlen = strlen(conn->resp_headers[i].value);
        memcpy(buf + off, conn->resp_headers[i].name, nlen);
        off += nlen;
        buf[off++] = ':';
        buf[off++] = ' ';
        memcpy(buf + off, conn->resp_headers[i].value, vlen);
        off += vlen;
        buf[off++] = '\r';
        buf[off++] = '\n';
    }

    /* Content-Length header. */
    if (!_http_srv_resp_header_has(conn, "Content-Length")) {
        memcpy(buf + off, "Content-Length: ", 16);
        off += 16;
        off += http_write_uint(buf + off, content_length);
        buf[off++] = '\r';
        buf[off++] = '\n';
    }

    buf[off++] = '\r';
    buf[off++] = '\n';

    conn->resp_headers_sent = true;
    *out_buf = buf;
    *out_len = off;
    return 0;
}

/*
 * Flush buffered response headers for streaming (write mode).
 * If user set Content-Length, sends as fixed-length (cl_mode).
 * Otherwise sends Transfer-Encoding: chunked.
 */
static int _http_srv_flush_resp_headers(xylem_http_conn_t* conn) {
    /* Auto-add Date header if not already set (RFC 9110 §6.6.1). */
    if (!_http_srv_resp_header_has(conn, "Date")) {
        char date_buf[32];
        time_t now = time(NULL);
        _http_format_date(now, date_buf, sizeof(date_buf));
        _http_srv_resp_header_set(conn, "Date", date_buf);
    }

    /* Auto-add Connection: close when not keeping alive (RFC 9112 §9.6). */
    if (!conn->keep_alive &&
        !_http_srv_resp_header_has(conn, "Connection")) {
        _http_srv_resp_header_set(conn, "Connection", "close");
    }

    bool has_cl = _http_srv_resp_header_has(conn, "Content-Length");

    size_t est = 40 + 2; /* status line + final CRLF */
    if (!has_cl) {
        est += 28; /* Transfer-Encoding: chunked\r\n */
    }
    for (size_t i = 0; i < conn->resp_header_count; i++) {
        if (conn->resp_headers[i].name && conn->resp_headers[i].value) {
            est += strlen(conn->resp_headers[i].name)
                 + strlen(conn->resp_headers[i].value) + 4;
        }
    }

    char* buf = malloc(est);
    if (!buf) {
        return -1;
    }

    const char* reason = http_reason_phrase(conn->resp_status);
    size_t reason_len = strlen(reason);
    size_t off = 0;

    memcpy(buf + off, "HTTP/1.1 ", 9);
    off += 9;
    buf[off++] = (char)('0' + conn->resp_status / 100);
    buf[off++] = (char)('0' + (conn->resp_status / 10) % 10);
    buf[off++] = (char)('0' + conn->resp_status % 10);
    buf[off++] = ' ';
    memcpy(buf + off, reason, reason_len);
    off += reason_len;
    buf[off++] = '\r';
    buf[off++] = '\n';

    if (!has_cl) {
        memcpy(buf + off, "Transfer-Encoding: chunked\r\n", 28);
        off += 28;
    }

    for (size_t i = 0; i < conn->resp_header_count; i++) {
        if (!conn->resp_headers[i].name || !conn->resp_headers[i].value) {
            continue;
        }
        size_t nlen = strlen(conn->resp_headers[i].name);
        size_t vlen = strlen(conn->resp_headers[i].value);
        memcpy(buf + off, conn->resp_headers[i].name, nlen);
        off += nlen;
        buf[off++] = ':';
        buf[off++] = ' ';
        memcpy(buf + off, conn->resp_headers[i].value, vlen);
        off += vlen;
        buf[off++] = '\r';
        buf[off++] = '\n';
    }

    buf[off++] = '\r';
    buf[off++] = '\n';

    int rc = conn->vt->send(conn->transport, buf, off);
    free(buf);

    if (rc == 0) {
        conn->resp_headers_sent = true;
        if (has_cl) {
            conn->cl_mode = true;
        } else {
            conn->chunked_active = true;
        }
    }
    return (rc == 0) ? 0 : -1;
}

int xylem_http_writer_set_header(xylem_http_writer_t* conn,
                               const char* name,
                               const char* value) {
    if (!conn || conn->closed || conn->resp_headers_sent) {
        return -1;
    }
    if (!name || !value) {
        return -1;
    }
    return _http_srv_resp_header_set(conn, name, value);
}

int xylem_http_writer_set_status(xylem_http_writer_t* conn,
                               int status_code) {
    if (!conn || conn->closed || conn->resp_headers_sent) {
        return -1;
    }
    conn->resp_status = status_code;
    return 0;
}

/* Forward declaration --send_chunk is defined below write(). */
static int _http_srv_send_chunk(xylem_http_conn_t* conn,
                                const void* data, size_t len);

int xylem_http_writer_write(xylem_http_writer_t* conn,
                          const void* data, size_t len) {
    if (!conn || conn->closed) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    /* First write: decide gzip / CL mode, flush headers. */
    if (!conn->resp_headers_sent) {
        if (_http_srv_should_gzip(conn)) {
            /* Init streaming gzip deflate. */
            mz_stream* s = calloc(1, sizeof(mz_stream));
            if (!s) {
                return -1;
            }
            /* windowBits = 15 + 16 for gzip wrapper format. */
            int rc = mz_deflateInit2(s, conn->srv->gzip_opts.level,
                                     MZ_DEFLATED, 15 + 16, 8,
                                     MZ_DEFAULT_STRATEGY);
            if (rc != MZ_OK) {
                free(s);
                return -1;
            }
            conn->gzip_stream = s;
            conn->gzip_active = true;
            _http_srv_resp_header_set(conn, "Content-Encoding", "gzip");
            _http_srv_vary_ensure(conn, "Accept-Encoding");
        }

        if (_http_srv_flush_resp_headers(conn) != 0) {
            return -1;
        }
    }

    /* CL mode: raw send without chunk framing. */
    if (conn->cl_mode) {
        return (conn->vt->send(conn->transport, data, len) == 0) ? 0 : -1;
    }

    /* Gzip active: deflate through stream, then send as chunk. */
    if (conn->gzip_active) {
        size_t bound = mz_deflateBound(conn->gzip_stream, (mz_ulong)len);
        uint8_t* cbuf = malloc(bound);
        if (!cbuf) {
            return -1;
        }
        conn->gzip_stream->next_in = (const unsigned char*)data;
        conn->gzip_stream->avail_in = (mz_uint32)len;
        conn->gzip_stream->next_out = cbuf;
        conn->gzip_stream->avail_out = (mz_uint32)bound;

        int rc = mz_deflate(conn->gzip_stream, MZ_SYNC_FLUSH);
        if (rc != MZ_OK) {
            free(cbuf);
            return -1;
        }
        size_t produced = bound - conn->gzip_stream->avail_out;
        int ret = -1;
        if (produced > 0) {
            ret = _http_srv_send_chunk(conn, cbuf, produced);
        } else {
            ret = 0;
        }
        free(cbuf);
        return ret;
    }

    /* Default chunked mode: send data as a chunk. */
    return _http_srv_send_chunk(conn, data, len);
}

static int _http_srv_send(xylem_http_conn_t* conn,
                          int status_code,
                          const char* content_type,
                          const void* body, size_t body_len,
                          const xylem_http_hdr_t* headers,
                          size_t header_count) {
    if (!conn || conn->closed) {
        return -1;
    }

    /* Buffer caller-supplied headers into writer state. */
    conn->resp_status = status_code;
    for (size_t i = 0; i < header_count; i++) {
        if (headers[i].name && headers[i].value) {
            if (_http_srv_resp_header_set(conn, headers[i].name,
                                          headers[i].value) != 0) {
                return -1;
            }
        }
    }

    /* Try gzip compression. */
    uint8_t* gzip_buf = NULL;
    size_t   gzip_len = 0;
    bool     gzipped  = _http_srv_try_gzip(conn->srv, &conn->req,
                                           content_type, body, body_len,
                                           conn->resp_headers,
                                           conn->resp_header_count,
                                           &gzip_buf, &gzip_len);
    if (gzipped) {
        body     = gzip_buf;
        body_len = gzip_len;
        _http_srv_resp_header_set(conn, "Content-Encoding", "gzip");
        _http_srv_vary_ensure(conn, "Accept-Encoding");
    }

    /* Set Content-Type if not overridden. */
    if (content_type && !_http_srv_resp_header_has(conn, "Content-Type")) {
        _http_srv_resp_header_set(conn, "Content-Type", content_type);
    }

    /* Flush headers with Content-Length. */
    char* head_buf = NULL;
    size_t head_len = 0;
    if (_http_srv_flush_resp_headers_cl(conn, body_len,
                                        &head_buf, &head_len) != 0) {
        free(gzip_buf);
        return -1;
    }

    /* Combine head + body into a single send. */
    size_t total = head_len + body_len;
    char* buf = malloc(total);
    if (!buf) {
        free(head_buf);
        free(gzip_buf);
        return -1;
    }
    memcpy(buf, head_buf, head_len);
    if (body && body_len > 0) {
        memcpy(buf + head_len, body, body_len);
    }
    free(head_buf);

    int rc = conn->vt->send(conn->transport, buf, total);
    free(buf);
    free(gzip_buf);
    return (rc == 0) ? 0 : -1;
}

static int _http_srv_send_partial(xylem_http_conn_t* conn,
                                  const char* content_type,
                                  const void* body, size_t body_len,
                                  size_t range_start, size_t range_end,
                                  size_t total_size,
                                  const xylem_http_hdr_t* headers,
                                  size_t header_count) {
    if (!conn || conn->closed) {
        return -1;
    }

    /* Validate range: start <= end < total_size. */
    bool valid = (range_start <= range_end && range_end < total_size);
    conn->resp_status = valid ? 206 : 416;

    /* Buffer caller-supplied headers into writer state. */
    for (size_t i = 0; i < header_count; i++) {
        if (headers[i].name && headers[i].value) {
            if (_http_srv_resp_header_set(conn, headers[i].name,
                                          headers[i].value) != 0) {
                return -1;
            }
        }
    }

    /* Build Content-Range value. */
    char cr_buf[80];
    if (valid) {
        size_t p = 0;
        memcpy(cr_buf, "bytes ", 6);
        p += 6;
        p += http_write_uint(cr_buf + p, range_start);
        cr_buf[p++] = '-';
        p += http_write_uint(cr_buf + p, range_end);
        cr_buf[p++] = '/';
        p += http_write_uint(cr_buf + p, total_size);
        cr_buf[p] = '\0';
    } else {
        size_t p = 0;
        memcpy(cr_buf, "bytes */", 8);
        p += 8;
        p += http_write_uint(cr_buf + p, total_size);
        cr_buf[p] = '\0';
    }

    if (!_http_srv_resp_header_has(conn, "Content-Range")) {
        _http_srv_resp_header_set(conn, "Content-Range", cr_buf);
    }
    if (content_type && !_http_srv_resp_header_has(conn, "Content-Type")) {
        _http_srv_resp_header_set(conn, "Content-Type", content_type);
    }

    size_t actual_body_len = valid ? body_len : 0;

    /* Flush headers with Content-Length. */
    char* head_buf = NULL;
    size_t head_len = 0;
    if (_http_srv_flush_resp_headers_cl(conn, actual_body_len,
                                        &head_buf, &head_len) != 0) {
        return -1;
    }

    /* Combine head + body into a single send. */
    size_t total = head_len + actual_body_len;
    char* buf = malloc(total);
    if (!buf) {
        free(head_buf);
        return -1;
    }
    memcpy(buf, head_buf, head_len);
    if (valid && body && body_len > 0) {
        memcpy(buf + head_len, body, body_len);
    }
    free(head_buf);

    int rc = conn->vt->send(conn->transport, buf, total);
    free(buf);
    return (rc == 0) ? 0 : -1;
}


static int _http_srv_begin_chunked(xylem_http_conn_t* conn,
                                   int status_code,
                                   const char* content_type,
                                   const xylem_http_hdr_t* headers,
                                   size_t header_count) {
    if (!conn || conn->closed || conn->chunked_active) {
        return -1;
    }

    /* Buffer caller-supplied headers into writer state. */
    conn->resp_status = status_code;
    for (size_t i = 0; i < header_count; i++) {
        if (headers[i].name && headers[i].value) {
            if (_http_srv_resp_header_set(conn, headers[i].name,
                                          headers[i].value) != 0) {
                return -1;
            }
        }
    }

    /* Set Content-Type if not overridden. */
    if (content_type && !_http_srv_resp_header_has(conn, "Content-Type")) {
        _http_srv_resp_header_set(conn, "Content-Type", content_type);
    }

    /* Flush as chunked response. */
    return _http_srv_flush_resp_headers(conn);
}

static int _http_srv_send_chunk(xylem_http_conn_t* conn,
                                const void* data, size_t len) {
    if (!conn || conn->closed || !conn->chunked_active) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    /* Format: {hex_size}\r\n{data}\r\n */
    char size_buf[20];
    size_t size_len = 0;
    /* Write hex digits in reverse, then flip. */
    {
        char tmp[16];
        size_t n = 0;
        size_t v = len;
        do {
            tmp[n++] = "0123456789abcdef"[v & 0xF];
            v >>= 4;
        } while (v > 0);
        for (size_t k = 0; k < n; k++) {
            size_buf[k] = tmp[n - 1 - k];
        }
        size_len = n;
    }
    size_buf[size_len++] = '\r';
    size_buf[size_len++] = '\n';

    size_t frame_len = size_len + len + 2;
    char* frame = malloc(frame_len);
    if (!frame) {
        return -1;
    }

    memcpy(frame, size_buf, size_len);
    memcpy(frame + size_len, data, len);
    frame[frame_len - 2] = '\r';
    frame[frame_len - 1] = '\n';

    int rc = conn->vt->send(conn->transport, frame, frame_len);
    free(frame);
    return (rc == 0) ? 0 : -1;
}

static int _http_srv_end_chunked(xylem_http_conn_t* conn) {
    if (!conn || conn->closed || !conn->chunked_active) {
        return -1;
    }

    static const char terminator[] = "0\r\n\r\n";
    int rc = conn->vt->send(conn->transport, terminator,
                            sizeof(terminator) - 1);
    conn->chunked_active = false;

    if (rc != 0) {
        return -1;
    }

    /* Handle keep-alive: prepare for next request or close. */
    _http_srv_conn_finish_response(conn);

    return 0;
}

static int _http_srv_begin_sse(xylem_http_conn_t* conn,
                               const xylem_http_hdr_t* headers,
                               size_t header_count) {
    /* Merge caller headers with SSE-required headers. */
    size_t extra = 2; /* Cache-Control + Connection */
    size_t total = header_count + extra;
    xylem_http_hdr_t* merged = malloc(total * sizeof(xylem_http_hdr_t));
    if (!merged) {
        return -1;
    }

    for (size_t i = 0; i < header_count; i++) {
        merged[i] = headers[i];
    }
    merged[header_count].name     = "Cache-Control";
    merged[header_count].value    = "no-cache";
    merged[header_count + 1].name  = "Connection";
    merged[header_count + 1].value = "keep-alive";

    int rc = _http_srv_begin_chunked(conn, 200,
                                      "text/event-stream",
                                      merged, total);
    free(merged);
    return rc;
}

char* xylem_http_sse_build(const char* event,
                          const char* data,
                          size_t* len) {
    if (!data) {
        return NULL;
    }

    size_t event_len = event ? strlen(event) : 0;
    size_t data_len  = strlen(data);

    /* Count newlines in data to determine number of data: lines. */
    size_t line_count = 1;
    for (size_t i = 0; i < data_len; i++) {
        if (data[i] == '\n') {
            line_count++;
        }
    }

    /* "event: {event}\n" + line_count * "data: " + data + newlines + "\n" */
    size_t est = (event ? 7 + event_len + 1 : 0)
               + line_count * 6  /* "data: " per line */
               + data_len
               + line_count      /* '\n' per line */
               + 1;              /* trailing '\n' for blank line */

    char* buf = malloc(est);
    if (!buf) {
        return NULL;
    }

    size_t off = 0;

    if (event) {
        memcpy(buf + off, "event: ", 7);
        off += 7;
        memcpy(buf + off, event, event_len);
        off += event_len;
        buf[off++] = '\n';
    }

    /* Split data by '\n' into multiple "data: " lines. */
    const char* p = data;
    const char* end = data + data_len;
    while (p <= end) {
        const char* nl = p;
        while (nl < end && *nl != '\n') {
            nl++;
        }
        size_t seg_len = (size_t)(nl - p);
        memcpy(buf + off, "data: ", 6);
        off += 6;
        if (seg_len > 0) {
            memcpy(buf + off, p, seg_len);
            off += seg_len;
        }
        buf[off++] = '\n';
        p = nl + 1;
    }

    /* Blank line terminates the event. */
    buf[off++] = '\n';

    if (len) {
        *len = off;
    }
    return buf;
}

static int _http_srv_end_sse(xylem_http_conn_t* conn) {
    return _http_srv_end_chunked(conn);
}

void xylem_http_writer_close(xylem_http_writer_t* conn) {
    if (!conn || conn->closed) {
        return;
    }
    conn->closed = true;
    conn->vt->close_conn(conn->transport);
}

int xylem_http_writer_accept_upgrade(xylem_http_writer_t* conn,
                                     void** transport) {
    const char* upgrade_val;
    char buf[256];
    int len;
    int rc;

    if (!conn || !transport) {
        return -1;
    }
    if (conn->closed) {
        return -1;
    }
    if (!conn->in_upgrade_cb) {
        return -1;
    }
    if (conn->upgrade_accepted) {
        return -1;
    }

    /* Get the Upgrade header value from the request. */
    upgrade_val = http_header_find(
        conn->req.headers, conn->req.header_count, "Upgrade");
    if (!upgrade_val) {
        upgrade_val = "websocket";
    }

    /* Build 101 Switching Protocols response. */
    len = snprintf(buf, sizeof(buf),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: %s\r\n"
        "Connection: Upgrade\r\n"
        "\r\n",
        upgrade_val);

    if (len < 0 || (size_t)len >= sizeof(buf)) {
        return -1;
    }

    /* Send the 101 response. */
    rc = conn->vt->send(conn->transport, buf, (size_t)len);
    if (rc != 0) {
        return -1;
    }

    /* Stop idle timer. */
    if (conn->idle_timer.active) {
        xylem_loop_stop_timer(&conn->idle_timer);
    }

    /* Transfer transport ownership to caller. */
    *transport = conn->transport;

    /* Clear userdata so the transport close callback won't use stale conn. */
    conn->vt->set_userdata(conn->transport, NULL);

    /* Detach from HTTP management. */
    conn->transport = NULL;
    conn->upgrade_accepted = true;
    conn->closed = true;

    return 0;
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

/* Route segment types. */
typedef enum {
    _ROUTE_SEG_LITERAL,  /* exact text match */
    _ROUTE_SEG_PARAM,    /* :name --captures one segment */
    _ROUTE_SEG_WILDCARD  /* * --matches rest of path */
} _route_seg_type_t;

typedef struct {
    _route_seg_type_t type;
    char*             text;  /* literal text or param name (without ':') */
    size_t            len;
} _route_seg_t;

typedef struct {
    char*                      method;   /* NULL = match all */
    _route_seg_t*              segs;
    size_t                     seg_count;
    xylem_http_on_request_fn_t handler;
    void*                      userdata;
} _http_route_t;

typedef struct {
    xylem_http_middleware_fn_t fn;
    void*                     userdata;
} _http_middleware_t;

struct xylem_http_router_s {
    _http_route_t*     routes;
    size_t             count;
    size_t             cap;
    _http_middleware_t* middlewares;
    size_t             mw_count;
    size_t             mw_cap;
};

static void _http_route_free(_http_route_t* r) {
    free(r->method);
    for (size_t i = 0; i < r->seg_count; i++) {
        free(r->segs[i].text);
    }
    free(r->segs);
}

/* Parse pattern into segments. Returns 0 on success. */
static int _http_route_parse(const char* pattern, _route_seg_t** out_segs,
                             size_t* out_count) {
    if (!pattern || pattern[0] != '/') {
        return -1;
    }

    size_t cap = 8;
    _route_seg_t* segs = malloc(cap * sizeof(_route_seg_t));
    if (!segs) {
        return -1;
    }
    size_t count = 0;

    const char* p = pattern + 1; /* skip leading '/' */

    while (*p) {
        if (count >= cap) {
            cap *= 2;
            _route_seg_t* tmp = realloc(segs, cap * sizeof(_route_seg_t));
            if (!tmp) {
                goto fail;
            }
            segs = tmp;
        }

        if (*p == '*') {
            segs[count].type = _ROUTE_SEG_WILDCARD;
            segs[count].text = NULL;
            segs[count].len  = 0;
            count++;
            break; /* wildcard consumes the rest */
        }

        const char* seg_start = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t seg_len = (size_t)(p - seg_start);

        if (seg_len > 0 && seg_start[0] == ':') {
            /* Path parameter. */
            segs[count].type = _ROUTE_SEG_PARAM;
            segs[count].len  = seg_len - 1;
            segs[count].text = malloc(seg_len);
            if (!segs[count].text) {
                goto fail;
            }
            memcpy(segs[count].text, seg_start + 1, seg_len - 1);
            segs[count].text[seg_len - 1] = '\0';
        } else {
            segs[count].type = _ROUTE_SEG_LITERAL;
            segs[count].len  = seg_len;
            segs[count].text = malloc(seg_len + 1);
            if (!segs[count].text) {
                goto fail;
            }
            memcpy(segs[count].text, seg_start, seg_len);
            segs[count].text[seg_len] = '\0';
        }
        count++;

        if (*p == '/') {
            p++;
        }
    }

    *out_segs  = segs;
    *out_count = count;
    return 0;

fail:
    for (size_t i = 0; i < count; i++) {
        free(segs[i].text);
    }
    free(segs);
    return -1;
}

static char* _http_srv_strdup(const char* s) {
    size_t len = strlen(s);
    char* d = malloc(len + 1);
    if (d) {
        memcpy(d, s, len + 1);
    }
    return d;
}

xylem_http_router_t* xylem_http_router_create(void) {
    xylem_http_router_t* r = calloc(1, sizeof(*r));
    return r;
}

void xylem_http_router_destroy(xylem_http_router_t* router) {
    if (!router) {
        return;
    }
    for (size_t i = 0; i < router->count; i++) {
        _http_route_free(&router->routes[i]);
    }
    free(router->routes);
    free(router->middlewares);
    free(router);
}

int xylem_http_router_use(xylem_http_router_t* router,
                          xylem_http_middleware_fn_t fn,
                          void* userdata) {
    if (!router || !fn) {
        return -1;
    }

    if (router->mw_count >= router->mw_cap) {
        size_t new_cap = router->mw_cap ? router->mw_cap * 2 : 4;
        _http_middleware_t* tmp = realloc(router->middlewares,
                                         new_cap * sizeof(_http_middleware_t));
        if (!tmp) {
            return -1;
        }
        router->middlewares = tmp;
        router->mw_cap = new_cap;
    }

    router->middlewares[router->mw_count].fn       = fn;
    router->middlewares[router->mw_count].userdata  = userdata;
    router->mw_count++;
    return 0;
}

int xylem_http_router_add(xylem_http_router_t* router,
                          const char* method,
                          const char* pattern,
                          xylem_http_on_request_fn_t handler,
                          void* userdata) {
    if (!router || !pattern || !handler) {
        return -1;
    }

    _route_seg_t* segs = NULL;
    size_t seg_count = 0;
    if (_http_route_parse(pattern, &segs, &seg_count) != 0) {
        return -1;
    }

    if (router->count >= router->cap) {
        size_t new_cap = router->cap ? router->cap * 2 : 8;
        _http_route_t* tmp = realloc(router->routes,
                                     new_cap * sizeof(_http_route_t));
        if (!tmp) {
            for (size_t i = 0; i < seg_count; i++) {
                free(segs[i].text);
            }
            free(segs);
            return -1;
        }
        router->routes = tmp;
        router->cap = new_cap;
    }

    _http_route_t* route = &router->routes[router->count];
    route->method    = method ? _http_srv_strdup(method) : NULL;
    route->segs      = segs;
    route->seg_count = seg_count;
    route->handler   = handler;
    route->userdata  = userdata;

    if (method && !route->method) {
        _http_route_free(route);
        return -1;
    }

    router->count++;
    return 0;
}

/*
 * Match score: higher is better.
 *   - Each literal segment: +3
 *   - Each param segment:   +2
 *   - Wildcard:             +1
 *   - Specific method match: +1000
 *   - NULL method (wildcard): +0
 */
typedef struct {
    size_t          route_idx;
    int             score;
    /* Captured params during matching. */
    http_header_t*  params;
    size_t          param_count;
} _route_match_t;

/**
 * Check if a route's path pattern matches the given path, ignoring method.
 * Used to distinguish 404 (no path match) from 405 (path matches, wrong method).
 */
static bool _http_route_path_matches(const _http_route_t* route,
                                     const char* path) {
    const char* p = path;
    if (*p == '/') {
        p++;
    }

    for (size_t i = 0; i < route->seg_count; i++) {
        const _route_seg_t* seg = &route->segs[i];
        if (seg->type == _ROUTE_SEG_WILDCARD) {
            return true;
        }
        const char* seg_start = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t seg_len = (size_t)(p - seg_start);
        if (seg_len == 0) {
            return false;
        }
        if (seg->type == _ROUTE_SEG_LITERAL) {
            if (seg_len != seg->len ||
                memcmp(seg_start, seg->text, seg_len) != 0) {
                return false;
            }
        }
        if (*p == '/') {
            p++;
        }
    }
    return (*p == '\0');
}

static int _http_router_try_match(const _http_route_t* route,
                                  const char* method,
                                  const char* path,
                                  _route_match_t* match) {
    /* Check method. */
    if (route->method && strcmp(route->method, method) != 0) {
        return -1;
    }

    int score = route->method ? 1000 : 0;

    const char* p = path;
    if (*p == '/') {
        p++;
    }

    /* Temporary param storage. */
    http_header_t params[16];
    size_t param_count = 0;

    for (size_t i = 0; i < route->seg_count; i++) {
        const _route_seg_t* seg = &route->segs[i];

        if (seg->type == _ROUTE_SEG_WILDCARD) {
            score += 1;
            /* Wildcard matches the rest --success. */
            match->score = score;
            match->params = NULL;
            match->param_count = 0;
            if (param_count > 0) {
                match->params = malloc(param_count * sizeof(http_header_t));
                if (match->params) {
                    for (size_t k = 0; k < param_count; k++) {
                        match->params[k] = params[k];
                    }
                    match->param_count = param_count;
                }
            }
            return 0;
        }

        /* Extract current path segment. */
        const char* seg_start = p;
        while (*p && *p != '/') {
            p++;
        }
        size_t seg_len = (size_t)(p - seg_start);

        if (seg_len == 0) {
            return -1; /* path too short */
        }

        if (seg->type == _ROUTE_SEG_LITERAL) {
            if (seg_len != seg->len || memcmp(seg_start, seg->text, seg_len) != 0) {
                return -1;
            }
            score += 3;
        } else if (seg->type == _ROUTE_SEG_PARAM) {
            score += 2;
            if (param_count < 16) {
                /* Store param --we'll copy properly if this route wins. */
                params[param_count].name  = seg->text;
                params[param_count].value = (char*)seg_start;
                param_count++;
            }
        }

        if (*p == '/') {
            p++;
        }
    }

    /* All segments matched --path must also be fully consumed. */
    if (*p != '\0') {
        return -1;
    }

    match->score = score;
    match->params = NULL;
    match->param_count = 0;

    if (param_count > 0) {
        match->params = malloc(param_count * sizeof(http_header_t));
        if (match->params) {
            match->param_count = param_count;
            /* Copy with proper allocation: name from seg->text,
               value needs to be extracted from path segment. */
            for (size_t k = 0; k < param_count; k++) {
                const char* val_start = params[k].value;
                /* Find end of this segment value. */
                const char* val_end = val_start;
                while (*val_end && *val_end != '/') {
                    val_end++;
                }
                size_t vlen = (size_t)(val_end - val_start);
                size_t nlen = strlen(params[k].name);
                /* Single allocation for name + value. */
                char* block = malloc(nlen + 1 + vlen + 1);
                if (block) {
                    memcpy(block, params[k].name, nlen);
                    block[nlen] = '\0';
                    memcpy(block + nlen + 1, val_start, vlen);
                    block[nlen + 1 + vlen] = '\0';
                    match->params[k].name  = block;
                    match->params[k].value = block + nlen + 1;
                } else {
                    match->params[k].name  = NULL;
                    match->params[k].value = NULL;
                }
            }
        }
    }

    return 0;
}

int xylem_http_router_dispatch(xylem_http_router_t* router,
                               xylem_http_writer_t* conn,
                               xylem_http_req_t* req) {
    if (!router || !conn || !req) {
        return -1;
    }

    const char* method = req->method;
    const char* path   = req->url;

    _route_match_t best;
    best.route_idx   = (size_t)-1;
    best.score       = -1;
    best.params      = NULL;
    best.param_count = 0;

    for (size_t i = 0; i < router->count; i++) {
        _route_match_t m;
        m.params = NULL;
        m.param_count = 0;
        if (_http_router_try_match(&router->routes[i], method, path, &m) == 0) {
            if (m.score > best.score) {
                /* Free previous best params. */
                for (size_t k = 0; k < best.param_count; k++) {
                    free(best.params[k].name);
                }
                free(best.params);
                best = m;
                best.route_idx = i;
            } else {
                /* Free this candidate's params. */
                for (size_t k = 0; k < m.param_count; k++) {
                    free(m.params[k].name);
                }
                free(m.params);
            }
        }
    }

    if (best.route_idx == (size_t)-1) {
        /* Check if any route matches the path (ignoring method)
           to distinguish 404 from 405 (RFC 9110 §15.5.6). */
        bool path_matched = false;
        char allow_buf[256];
        size_t allow_off = 0;

        for (size_t i = 0; i < router->count; i++) {
            if (_http_route_path_matches(&router->routes[i], path)) {
                path_matched = true;
                if (router->routes[i].method) {
                    const char* m = router->routes[i].method;
                    size_t mlen = strlen(m);
                    /* Avoid duplicates in allow_buf. */
                    bool dup = false;
                    if (allow_off > 0) {
                        const char* s = allow_buf;
                        while (s < allow_buf + allow_off) {
                            const char* comma = strchr(s, ',');
                            size_t slen = comma ? (size_t)(comma - s) : strlen(s);
                            if (slen == mlen && memcmp(s, m, mlen) == 0) {
                                dup = true;
                                break;
                            }
                            s = comma ? comma + 2 : s + slen;
                        }
                    }
                    if (!dup && allow_off + mlen + 3 < sizeof(allow_buf)) {
                        if (allow_off > 0) {
                            allow_buf[allow_off++] = ',';
                            allow_buf[allow_off++] = ' ';
                        }
                        memcpy(allow_buf + allow_off, m, mlen);
                        allow_off += mlen;
                    }
                }
            }
        }

        if (path_matched && allow_off > 0) {
            allow_buf[allow_off] = '\0';

            /* Auto-handle OPTIONS requests (RFC 9110 §9.3.7). */
            if (strcmp(method, "OPTIONS") == 0) {
                xylem_http_hdr_t hdr = { "Allow", allow_buf };
                _http_srv_send(conn, 204, NULL, NULL, 0, &hdr, 1);
                return 0;
            }

            xylem_http_hdr_t hdr = { "Allow", allow_buf };
            _http_srv_send(conn, 405, "text/plain",
                                 "Method Not Allowed", 18, &hdr, 1);
        } else {
            _http_srv_send(conn, 404, "text/plain",
                                 "Not Found", 9, NULL, 0);
        }
        return -1;
    }

    /* Attach params to req. */
    req->params      = best.params;
    req->param_count = best.param_count;

    /* Run middleware chain. */
    for (size_t i = 0; i < router->mw_count; i++) {
        if (router->middlewares[i].fn(conn, req,
                                      router->middlewares[i].userdata) != 0) {
            return -1;
        }
    }

    _http_route_t* route = &router->routes[best.route_idx];
    route->handler(conn, req, route->userdata);

    return 0;
}

static const char* _http_static_mime(const char* path) {
    const char* dot = NULL;
    for (const char* p = path; *p; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    if (!dot) {
        return "application/octet-stream";
    }
    for (size_t i = 0; _mime_map[i].ext; i++) {
        if (http_header_eq(dot, _mime_map[i].ext)) {
            return _mime_map[i].mime;
        }
    }
    return "application/octet-stream";
}

/**
 * Normalize a relative path: collapse ".." and "." segments,
 * reject any traversal that escapes the root. Returns a malloc'd
 * normalized path or NULL if unsafe.
 */
static char* _http_static_normalize(const char* rel) {
    if (!rel || *rel == '\0') {
        return NULL;
    }

    /* Reject absolute paths and backslash. */
    if (rel[0] == '/' || rel[0] == '\\') {
        return NULL;
    }
    for (const char* p = rel; *p; p++) {
        if (*p == '\\') {
            return NULL;
        }
    }

    size_t len = strlen(rel);
    char* buf = malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, rel, len + 1);

    /* Split into segments and resolve in-place. */
    char* segs[256];
    size_t depth = 0;
    char* tok = buf;

    while (*tok) {
        /* Skip leading slashes. */
        while (*tok == '/') {
            *tok++ = '\0';
        }
        if (*tok == '\0') {
            break;
        }
        char* seg = tok;
        while (*tok && *tok != '/') {
            tok++;
        }
        if (*tok == '/') {
            *tok++ = '\0';
        }

        if (strcmp(seg, ".") == 0) {
            continue;
        }
        if (strcmp(seg, "..") == 0) {
            if (depth == 0) {
                /* Traversal beyond root. */
                free(buf);
                return NULL;
            }
            depth--;
            continue;
        }
        if (depth >= 256) {
            free(buf);
            return NULL;
        }
        segs[depth++] = seg;
    }

    if (depth == 0) {
        /* Empty path after normalization --root directory request. */
        free(buf);
        char* empty = malloc(1);
        if (empty) {
            empty[0] = '\0';
        }
        return empty;
    }

    /* Rebuild normalized path. */
    size_t total = 0;
    for (size_t i = 0; i < depth; i++) {
        total += strlen(segs[i]) + 1; /* seg + '/' or '\0' */
    }
    char* out = malloc(total);
    if (!out) {
        free(buf);
        return NULL;
    }
    size_t off = 0;
    for (size_t i = 0; i < depth; i++) {
        size_t slen = strlen(segs[i]);
        memcpy(out + off, segs[i], slen);
        off += slen;
        if (i + 1 < depth) {
            out[off++] = '/';
        }
    }
    out[off] = '\0';
    free(buf);
    return out;
}

/* Build full filesystem path: root + "/" + rel. Caller frees. */
static char* _http_static_fullpath(const char* root, const char* rel) {
    size_t rlen = strlen(root);
    size_t plen = strlen(rel);
    /* Trim trailing separator from root. */
    while (rlen > 0 && (root[rlen - 1] == '/' || root[rlen - 1] == '\\')) {
        rlen--;
    }
    size_t need = rlen + 1 + plen + 1;
    char* path = malloc(need);
    if (!path) {
        return NULL;
    }
    memcpy(path, root, rlen);
    path[rlen] = '/';
    memcpy(path + rlen + 1, rel, plen);
    path[rlen + 1 + plen] = '\0';
    return path;
}

/* Format time_t as HTTP-date (RFC 7231). buf must be >= 30 bytes. */
static size_t _http_format_date(time_t t, char* buf, size_t cap) {
    struct tm gmt;
    platform_info_gmtime(&t, &gmt);
    return (size_t)snprintf(buf, cap,
        "%s, %02d %s %04d %02d:%02d:%02d GMT",
        _http_day_names[gmt.tm_wday],
        gmt.tm_mday,
        _http_month_names[gmt.tm_mon],
        gmt.tm_year + 1900,
        gmt.tm_hour, gmt.tm_min, gmt.tm_sec);
}

/* Parse HTTP-date (RFC 7231) to time_t. Returns -1 on failure. */
static time_t _http_parse_date(const char* str) {
    if (!str) {
        return (time_t)-1;
    }
    struct tm tm;
    memset(&tm, 0, sizeof(tm));

    /* Skip day name + ", " */
    const char* p = strchr(str, ',');
    if (!p) {
        return (time_t)-1;
    }
    p++; /* skip comma */
    while (*p == ' ') {
        p++;
    }

    /* Parse "DD Mon YYYY HH:MM:SS GMT" */
    char mon[4] = {0};
    int n = sscanf(p, "%d %3s %d %d:%d:%d",
                   &tm.tm_mday, mon,
                   &tm.tm_year, &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    if (n < 6) {
        return (time_t)-1;
    }

    tm.tm_year -= 1900;
    tm.tm_mon = -1;
    for (int i = 0; i < 12; i++) {
        if (mon[0] == _http_month_names[i][0] &&
            mon[1] == _http_month_names[i][1] &&
            mon[2] == _http_month_names[i][2]) {
            tm.tm_mon = i;
            break;
        }
    }
    if (tm.tm_mon < 0) {
        return (time_t)-1;
    }

    return platform_info_mkgmtime(&tm);
}

/**
 * Resolve a request URL to a filesystem path. Strips the mount prefix,
 * normalizes against traversal, appends index file for directories.
 * Returns malloc'd path and fills *st, or returns NULL (caller sends error).
 * *status receives the HTTP error code when NULL is returned.
 */
static char* _http_static_resolve(const void* userdata,
                                  const char* url,
                                  const char* index_file,
                                  platform_io_stat_t* st,
                                  int* status) {
    const size_t* prefix_len_ptr =
        (const size_t*)((const uint8_t*)userdata +
                        sizeof(xylem_http_static_opts_t));
    size_t prefix_len = *prefix_len_ptr;
    const xylem_http_static_opts_t* opts =
        (const xylem_http_static_opts_t*)userdata;

    const char* rel = url;
    if (strlen(url) >= prefix_len) {
        rel = url + prefix_len;
    }
    while (*rel == '/') {
        rel++;
    }

    const char* lookup = (*rel == '\0') ? index_file : rel;

    char* norm = _http_static_normalize(lookup);
    if (!norm) {
        *status = 403;
        return NULL;
    }

    char* fpath = _http_static_fullpath(opts->root, norm);
    free(norm);
    if (!fpath) {
        *status = 500;
        return NULL;
    }

    if (platform_io_stat(fpath, st) != 0) {
        free(fpath);
        *status = 404;
        return NULL;
    }

    /* Directory: try index file inside it. */
    if (st->is_dir) {
        size_t fplen = strlen(fpath);
        size_t ilen  = strlen(index_file);
        char* ipath  = malloc(fplen + 1 + ilen + 1);
        if (!ipath) {
            free(fpath);
            *status = 500;
            return NULL;
        }
        memcpy(ipath, fpath, fplen);
        ipath[fplen] = '/';
        memcpy(ipath + fplen + 1, index_file, ilen);
        ipath[fplen + 1 + ilen] = '\0';
        free(fpath);
        fpath = ipath;

        if (platform_io_stat(fpath, st) != 0) {
            free(fpath);
            *status = 403;
            return NULL;
        }
    }

    return fpath;
}

/**
 * Probe for a pre-compressed .gz variant. If found, replaces *fpath and *st
 * with the .gz version and returns true.
 */
static bool _http_static_try_gz(const xylem_http_req_t* req,
                                bool precompressed,
                                char** fpath,
                                platform_io_stat_t* st) {
    if (!precompressed) {
        return false;
    }
    const char* ae = http_header_find(req->headers, req->header_count,
                                      "Accept-Encoding");
    if (!ae || !strstr(ae, "gzip")) {
        return false;
    }

    size_t fplen = strlen(*fpath);
    char* gz_path = malloc(fplen + 4);
    if (!gz_path) {
        return false;
    }
    memcpy(gz_path, *fpath, fplen);
    memcpy(gz_path + fplen, ".gz", 4);

    platform_io_stat_t gz_st;
    if (platform_io_stat(gz_path, &gz_st) != 0) {
        free(gz_path);
        return false;
    }

    free(*fpath);
    *fpath = gz_path;
    *st    = gz_st;
    return true;
}

/**
 * Determine MIME type. For .gz paths, strips the suffix first so the
 * original extension is used.
 */
static const char* _http_static_content_type(const char* fpath, bool is_gz) {
    if (!is_gz) {
        return _http_static_mime(fpath);
    }
    size_t fplen = strlen(fpath);
    if (fplen <= 3) {
        return _http_static_mime(fpath);
    }
    char* orig = malloc(fplen - 2);
    if (!orig) {
        return _http_static_mime(fpath);
    }
    memcpy(orig, fpath, fplen - 3);
    orig[fplen - 3] = '\0';
    const char* mime = _http_static_mime(orig);
    free(orig);
    return mime;
}

/**
 * Parse a Range header value against a known file size.
 * Returns true if a valid byte range was parsed.
 */
static bool _http_static_parse_range(const char* hdr, size_t file_size,
                                     size_t* out_start, size_t* out_end) {
    if (!hdr || file_size == 0) {
        return false;
    }
    if (strncmp(hdr, "bytes=", 6) != 0) {
        return false;
    }
    const char* p = hdr + 6;

    if (*p == '-') {
        /* Suffix range: bytes=-N means last N bytes. */
        p++;
        size_t suffix = 0;
        while (*p >= '0' && *p <= '9') {
            suffix = suffix * 10 + (size_t)(*p - '0');
            p++;
        }
        if (suffix > 0 && suffix <= file_size) {
            *out_start = file_size - suffix;
            *out_end   = file_size - 1;
            return true;
        }
        return false;
    }

    if (*p < '0' || *p > '9') {
        return false;
    }

    size_t start = 0;
    while (*p >= '0' && *p <= '9') {
        start = start * 10 + (size_t)(*p - '0');
        p++;
    }
    if (*p != '-') {
        return false;
    }
    p++;

    size_t end;
    if (*p >= '0' && *p <= '9') {
        end = 0;
        while (*p >= '0' && *p <= '9') {
            end = end * 10 + (size_t)(*p - '0');
            p++;
        }
    } else {
        end = file_size - 1;
    }

    if (start <= end && end < file_size) {
        *out_start = start;
        *out_end   = end;
        return true;
    }
    return false;
}

/**
 * Read file content. For partial reads, seeks to offset first.
 * Returns malloc'd buffer or NULL on error. Closes fp before returning.
 */
static uint8_t* _http_static_read_file(FILE* fp, size_t offset, size_t len) {
    uint8_t* data = malloc(len);
    if (!data) {
        fclose(fp);
        return NULL;
    }
    if (offset > 0 && fseek(fp, (long)offset, SEEK_SET) != 0) {
        free(data);
        fclose(fp);
        return NULL;
    }
    if (fread(data, 1, len, fp) != len) {
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    return data;
}

/* Static file request handler --registered as router callback. */
static void _http_static_handler(xylem_http_conn_t* conn,
                                 xylem_http_req_t* req,
                                 void* userdata) {
    const xylem_http_static_opts_t* opts =
        (const xylem_http_static_opts_t*)userdata;

    const char* method = req->method;
    bool is_get  = (strcmp(method, "GET") == 0);
    bool is_head = (strcmp(method, "HEAD") == 0);
    if (!is_get && !is_head) {
        xylem_http_hdr_t allow = { "Allow", "GET, HEAD" };
        _http_srv_send(conn, 405, "text/plain",
                             "Method Not Allowed", 18, &allow, 1);
        return;
    }

    const char* index_file = opts->index_file ? opts->index_file : "index.html";

    /* Resolve URL to filesystem path. */
    platform_io_stat_t st;
    int err_status = 0;
    char* fpath = _http_static_resolve(userdata, req->url, index_file,
                                       &st, &err_status);
    if (!fpath) {
        const char* msg = (err_status == 404) ? "Not Found" :
                          (err_status == 403) ? "Forbidden" :
                                                "Internal Server Error";
        size_t mlen = strlen(msg);
        _http_srv_send(conn, err_status, "text/plain", msg, mlen, NULL, 0);
        return;
    }

    /* Check If-Modified-Since. */
    const char* ims = http_header_find(req->headers, req->header_count,
                                       "If-Modified-Since");
    if (ims) {
        time_t ims_time = _http_parse_date(ims);
        if (ims_time != (time_t)-1 && st.mtime <= ims_time) {
            free(fpath);
            _http_srv_send(conn, 304, NULL, NULL, 0, NULL, 0);
            return;
        }
    }

    /* Try pre-compressed .gz version. */
    bool serve_gz = _http_static_try_gz(req, opts->precompressed,
                                        &fpath, &st);

    const char* mime = _http_static_content_type(fpath, serve_gz);
    size_t file_size = (size_t)st.size;

    /* Build common response headers. */
    xylem_http_hdr_t hdrs[5];
    size_t hdr_count = 0;

    char date_buf[32];
    _http_format_date(st.mtime, date_buf, sizeof(date_buf));
    hdrs[hdr_count].name  = "Last-Modified";
    hdrs[hdr_count].value = date_buf;
    hdr_count++;

    char cache_buf[48];
    if (opts->max_age > 0) {
        snprintf(cache_buf, sizeof(cache_buf),
                 "public, max-age=%d", opts->max_age);
        hdrs[hdr_count].name  = "Cache-Control";
        hdrs[hdr_count].value = cache_buf;
        hdr_count++;
    }

    if (serve_gz) {
        hdrs[hdr_count].name  = "Content-Encoding";
        hdrs[hdr_count].value = "gzip";
        hdr_count++;
    }

    if (!serve_gz) {
        hdrs[hdr_count].name  = "Accept-Ranges";
        hdrs[hdr_count].value = "bytes";
        hdr_count++;
    }

    /* Parse Range header (only for non-gz responses). */
    const char* range_hdr = serve_gz ? NULL :
        http_header_find(req->headers, req->header_count, "Range");

    size_t range_start = 0;
    size_t range_end   = 0;
    bool has_range = _http_static_parse_range(range_hdr, file_size,
                                              &range_start, &range_end);

    /* Open file. */
    FILE* fp = platform_io_fopen(fpath, "rb");
    if (!fp) {
        free(fpath);
        _http_srv_send(conn, 500, "text/plain",
                             "Internal Server Error", 21, NULL, 0);
        return;
    }

    if (has_range) {
        size_t slice_len = range_end - range_start + 1;
        uint8_t* data = NULL;
        if (!is_head) {
            data = _http_static_read_file(fp, range_start, slice_len);
            if (!data) {
                free(fpath);
                _http_srv_send(conn, 500, "text/plain",
                                     "Internal Server Error", 21, NULL, 0);
                return;
            }
        } else {
            fclose(fp);
        }
        /* HEAD: set Content-Length to actual slice size. */
        if (is_head) {
            char cl_buf[24];
            size_t cl_len = http_write_uint(cl_buf, slice_len);
            cl_buf[cl_len] = '\0';
            _http_srv_resp_header_set(conn, "Content-Length", cl_buf);
        }
        _http_srv_send_partial(conn, mime,
                                     is_head ? NULL : data,
                                     is_head ? 0 : slice_len,
                                     range_start, range_end, file_size,
                                     hdrs, hdr_count);
        free(data);
    } else {
        uint8_t* data = NULL;
        if (!is_head) {
            data = _http_static_read_file(fp, 0, file_size);
            if (!data) {
                free(fpath);
                _http_srv_send(conn, 500, "text/plain",
                                     "Internal Server Error", 21, NULL, 0);
                return;
            }
        } else {
            fclose(fp);
        }
        /* HEAD: set Content-Length to actual file size, send no body. */
        if (is_head) {
            char cl_buf[24];
            size_t cl_len = http_write_uint(cl_buf, file_size);
            cl_buf[cl_len] = '\0';
            _http_srv_resp_header_set(conn, "Content-Length", cl_buf);
        }
        _http_srv_send(conn, 200, mime,
                             is_head ? NULL : data,
                             is_head ? 0 : file_size,
                             hdrs, hdr_count);
        free(data);
    }

    free(fpath);
}

int xylem_http_static_serve(xylem_http_router_t* router,
                            const char* prefix,
                            const xylem_http_static_opts_t* opts) {
    if (!router || !prefix || !opts || !opts->root) {
        return -1;
    }

    /* Build the wildcard pattern: prefix + "/*" if not already. */
    size_t plen = strlen(prefix);
    /* Trim trailing slash. */
    while (plen > 0 && prefix[plen - 1] == '/') {
        plen--;
    }

    /* Calculate prefix_len for URL stripping (without "/*"). */
    size_t prefix_len = plen;
    /* Ensure prefix starts with '/'. */
    if (plen == 0 || prefix[0] != '/') {
        return -1;
    }

    char* pattern = malloc(plen + 3); /* "/prefix/*\0" */
    if (!pattern) {
        return -1;
    }
    memcpy(pattern, prefix, plen);
    pattern[plen]     = '/';
    pattern[plen + 1] = '*';
    pattern[plen + 2] = '\0';

    /* Allocate userdata block: opts copy + prefix_len. */
    size_t ud_size = sizeof(xylem_http_static_opts_t) + sizeof(size_t);
    uint8_t* ud = malloc(ud_size);
    if (!ud) {
        free(pattern);
        return -1;
    }
    memcpy(ud, opts, sizeof(xylem_http_static_opts_t));
    memcpy(ud + sizeof(xylem_http_static_opts_t), &prefix_len, sizeof(size_t));

    int rc = xylem_http_router_add(router, NULL, pattern,
                                   _http_static_handler, ud);
    free(pattern);
    if (rc != 0) {
        free(ud);
        return -1;
    }
    /* Note: ud is leaked when router is destroyed. A production
       implementation would track it for cleanup. For now this is
       acceptable as the router lifetime matches the server. */
    return 0;
}
