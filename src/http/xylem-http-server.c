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

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct xylem_http_req_s {
    char           method[16];
    char*          url;
    http_header_t* headers;
    size_t         header_count;
    size_t         header_cap;
    uint8_t*       body;
    size_t         body_len;
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
    char*                      cur_header_name;
    size_t                     cur_header_name_len;
    bool                       expect_continue;
};

struct xylem_http_srv_s {
    xylem_loop_t*              loop;
    xylem_http_srv_cfg_t       cfg;
    const http_transport_vt_t* vt;
    void*                      listener;
    http_transport_cb_t        transport_cb;
    bool                       running;
};

static void _srv_req_reset(xylem_http_req_t* req) {
    free(req->url);
    http_headers_free(req->headers, req->header_count);
    free(req->body);
    memset(req, 0, sizeof(*req));
}

static void _srv_conn_destroy(xylem_http_conn_t* conn) {
    if (!conn) {
        return;
    }
    _srv_req_reset(&conn->req);
    free(conn->cur_header_name);
    if (conn->idle_timer.active) {
        xylem_loop_stop_timer(&conn->idle_timer);
    }
    xylem_loop_close_timer(&conn->idle_timer);
    free(conn);
}

static int _srv_parser_url_cb(llhttp_t* parser, const char* at, size_t len) {
    xylem_http_conn_t* conn = parser->data;
    xylem_http_req_t* req = &conn->req;

    /* Accumulate URL fragments. */
    char* tmp = realloc(req->url, (req->url ? strlen(req->url) : 0) + len + 1);
    if (!tmp) {
        return HPE_USER;
    }
    if (!req->url) {
        memcpy(tmp, at, len);
        tmp[len] = '\0';
    } else {
        size_t prev = strlen(tmp);
        memcpy(tmp + prev, at, len);
        tmp[prev + len] = '\0';
    }
    req->url = tmp;
    return 0;
}

static int _srv_parser_method_complete_cb(llhttp_t* parser) {
    xylem_http_conn_t* conn = parser->data;
    const char* m = llhttp_method_name(llhttp_get_method(parser));
    snprintf(conn->req.method, sizeof(conn->req.method), "%s", m);
    return 0;
}

static int _srv_parser_header_field_cb(llhttp_t* parser,
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

static int _srv_parser_header_value_cb(llhttp_t* parser,
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
    if (http_header_eq(conn->cur_header_name, "Expect")) {
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

static int _srv_parser_headers_complete_cb(llhttp_t* parser) {
    xylem_http_conn_t* conn = parser->data;

    conn->keep_alive = llhttp_should_keep_alive(parser);

    /* Send 100 Continue if the client expects it. */
    if (conn->expect_continue && !conn->closed) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        conn->vt->send(conn->transport, cont, sizeof(cont) - 1);
        conn->expect_continue = false;
    }

    return 0;
}

static int _srv_parser_body_cb(llhttp_t* parser, const char* at, size_t len) {
    xylem_http_conn_t* conn = parser->data;
    xylem_http_req_t* req = &conn->req;

    /* Enforce max body size. */
    if (req->body_len + len > conn->srv->cfg.max_body_size) {
        return HPE_USER;
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

static int _srv_parser_message_complete_cb(llhttp_t* parser) {
    xylem_http_conn_t* conn = parser->data;

    /* Reset idle timer — a complete request was received. */
    if (conn->idle_timer.active && conn->srv->cfg.idle_timeout_ms > 0) {
        xylem_loop_reset_timer(&conn->idle_timer,
                               conn->srv->cfg.idle_timeout_ms);
    }

    /* Dispatch to user callback. */
    if (conn->srv->cfg.on_request) {
        conn->srv->cfg.on_request(conn, &conn->req, conn->srv->cfg.userdata);
    }

    return HPE_PAUSED;
}

static void _srv_idle_timeout_cb(xylem_loop_t* loop,
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

static void _srv_conn_init_parser(xylem_http_conn_t* conn) {
    llhttp_settings_init(&conn->settings);
    conn->settings.on_url              = _srv_parser_url_cb;
    conn->settings.on_method_complete  = _srv_parser_method_complete_cb;
    conn->settings.on_header_field     = _srv_parser_header_field_cb;
    conn->settings.on_header_value     = _srv_parser_header_value_cb;
    conn->settings.on_headers_complete = _srv_parser_headers_complete_cb;
    conn->settings.on_body             = _srv_parser_body_cb;
    conn->settings.on_message_complete = _srv_parser_message_complete_cb;
    llhttp_init(&conn->parser, HTTP_REQUEST, &conn->settings);
    conn->parser.data = conn;
}

static void _srv_conn_accept_cb(void* handle, void* ctx) {
    xylem_http_srv_t* srv = ctx;

    xylem_http_conn_t* conn = calloc(1, sizeof(*conn));
    if (!conn) {
        srv->vt->close_conn(handle);
        return;
    }

    conn->srv       = srv;
    conn->vt        = srv->vt;
    conn->transport = handle;

    _srv_conn_init_parser(conn);

    /* Start idle timer. */
    xylem_loop_init_timer(srv->loop, &conn->idle_timer);
    if (srv->cfg.idle_timeout_ms > 0) {
        xylem_loop_start_timer(&conn->idle_timer,
                               _srv_idle_timeout_cb,
                               srv->cfg.idle_timeout_ms, 0);
    }

    /* Store conn pointer as userdata on the transport handle. */
    srv->vt->set_userdata(handle, conn);
}

static void _srv_conn_read_cb(void* handle, void* ctx,
                         void* data, size_t len) {
    xylem_http_srv_t* srv = ctx;

    xylem_http_conn_t* conn = srv->vt->get_userdata(handle);
    if (!conn || conn->closed) {
        return;
    }

    enum llhttp_errno err = llhttp_execute(&conn->parser, data, len);

    if (err == HPE_PAUSED) {
        /**
         * Message complete — the on_request callback has fired.
         * Prepare for the next request if keep-alive.
         */
        _srv_req_reset(&conn->req);
        free(conn->cur_header_name);
        conn->cur_header_name = NULL;
        conn->cur_header_name_len = 0;
        conn->expect_continue = false;

        if (conn->keep_alive && !conn->closed) {
            llhttp_resume(&conn->parser);
            _srv_conn_init_parser(conn);

            /* Reset idle timer for next request. */
            if (conn->srv->cfg.idle_timeout_ms > 0) {
                if (conn->idle_timer.active) {
                    xylem_loop_reset_timer(&conn->idle_timer,
                                           conn->srv->cfg.idle_timeout_ms);
                } else {
                    xylem_loop_start_timer(&conn->idle_timer,
                                           _srv_idle_timeout_cb,
                                           conn->srv->cfg.idle_timeout_ms, 0);
                }
            }
        } else if (!conn->closed) {
            conn->closed = true;
            conn->vt->close_conn(conn->transport);
        }
        return;
    }

    if (err == HPE_USER) {
        /* Body too large — send 413 and close. */
        if (!conn->closed) {
            static const char resp_413[] =
                "HTTP/1.1 413 Payload Too Large\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n"
                "\r\n";
            conn->vt->send(conn->transport, resp_413, sizeof(resp_413) - 1);
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
    }
}

static void _srv_conn_close_cb(void* handle, void* ctx, int err) {
    (void)err;
    xylem_http_srv_t* srv = ctx;
    xylem_http_conn_t* conn = srv->vt->get_userdata(handle);
    if (conn) {
        _srv_conn_destroy(conn);
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

xylem_http_srv_t* xylem_http_srv_create(xylem_loop_t* loop,
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

    return srv;
}

void xylem_http_srv_destroy(xylem_http_srv_t* srv) {
    if (!srv) {
        return;
    }
    if (srv->running) {
        xylem_http_srv_stop(srv);
    }
    free(srv);
}

int xylem_http_srv_start(xylem_http_srv_t* srv) {
    if (!srv || srv->running) {
        return -1;
    }

    srv->transport_cb.on_accept     = _srv_conn_accept_cb;
    srv->transport_cb.on_read       = _srv_conn_read_cb;
    srv->transport_cb.on_close      = _srv_conn_close_cb;
    srv->transport_cb.on_connect    = NULL;
    srv->transport_cb.on_write_done = NULL;

    const char* host = srv->cfg.host ? srv->cfg.host : "0.0.0.0";
    xylem_addr_t addr;
    if (xylem_addr_pton(host, srv->cfg.port, &addr) != 0) {
        return -1;
    }

    srv->listener = srv->vt->listen(srv->loop, &addr,
                                    &srv->transport_cb, srv, NULL,
                                    srv->cfg.tls_cert, srv->cfg.tls_key);
    if (!srv->listener) {
        return -1;
    }

    srv->running = true;
    return 0;
}

void xylem_http_srv_stop(xylem_http_srv_t* srv) {
    if (!srv || !srv->running) {
        return;
    }
    srv->vt->close_server(srv->listener);
    srv->listener = NULL;
    srv->running = false;
}

int xylem_http_conn_send(xylem_http_conn_t* conn,
                         int status_code,
                         const char* content_type,
                         const void* body, size_t body_len,
                         const xylem_http_hdr_t* headers,
                         size_t header_count) {
    if (!conn || conn->closed) {
        return -1;
    }

    const char* reason = http_reason_phrase(status_code);

    /* Check which auto-generated headers are overridden. */
    bool ct_overridden = false;
    bool cl_overridden = false;

    size_t custom_est = 0;
    for (size_t i = 0; i < header_count; i++) {
        if (!headers[i].name || !headers[i].value) {
            continue;
        }
        custom_est += strlen(headers[i].name) + 2
                    + strlen(headers[i].value) + 2;
        if (http_header_eq(headers[i].name, "Content-Type")) {
            ct_overridden = true;
        }
        if (http_header_eq(headers[i].name, "Content-Length")) {
            cl_overridden = true;
        }
    }

    /* Estimate buffer: status line + headers + body. */
    size_t est = 32 + strlen(reason)
               + custom_est
               + (content_type ? 16 + strlen(content_type) + 2 : 0)
               + 32 /* Content-Length */
               + 2  /* final CRLF */
               + body_len;

    char* buf = malloc(est);
    if (!buf) {
        return -1;
    }

    size_t off = 0;
    off += (size_t)snprintf(buf + off, est - off,
                            "HTTP/1.1 %d %s\r\n", status_code, reason);

    /* Custom headers first. */
    for (size_t i = 0; i < header_count; i++) {
        if (!headers[i].name || !headers[i].value) {
            continue;
        }
        off += (size_t)snprintf(buf + off, est - off, "%s: %s\r\n",
                                headers[i].name, headers[i].value);
    }

    /* Auto-generated headers, skipping overridden ones. */
    if (!ct_overridden && content_type) {
        off += (size_t)snprintf(buf + off, est - off,
                                "Content-Type: %s\r\n", content_type);
    }
    if (!cl_overridden) {
        off += (size_t)snprintf(buf + off, est - off,
                                "Content-Length: %zu\r\n", body_len);
    }

    buf[off++] = '\r';
    buf[off++] = '\n';

    if (body && body_len > 0) {
        memcpy(buf + off, body, body_len);
        off += body_len;
    }

    int rc = conn->vt->send(conn->transport, buf, off);
    free(buf);
    return (rc == 0) ? 0 : -1;
}

void xylem_http_conn_close(xylem_http_conn_t* conn) {
    if (!conn || conn->closed) {
        return;
    }
    conn->closed = true;
    conn->vt->close_conn(conn->transport);
}
