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

#include "xylem/ws/xylem-ws-server.h"
#include "xylem/xylem-addr.h"

#include "ws-common.h"
#include "ws-handshake.h"
#include "ws-transport.h"

#include "llhttp.h"

#include <stdlib.h>
#include <string.h>

struct xylem_ws_server_s {
    xylem_loop_t*            loop;
    const ws_transport_vt_t* vt;
    void*                    listener;
    ws_transport_cb_t        transport_cb;
    xylem_ws_handler_t*      handler;
    xylem_ws_opts_t          opts;
    bool                     running;
};

static void _ws_srv_send_http_error(xylem_ws_conn_t* conn,
                                    const char* response, size_t len) {
    conn->vt->send(conn->transport, response, len);
    conn->vt->close_conn(conn->transport);
}

static int _ws_srv_http_header_field_cb(llhttp_t* parser,
                                        const char* at, size_t len) {
    xylem_ws_conn_t* conn = parser->data;
    conn->current_header_field = at;
    conn->current_header_field_len = len;
    return 0;
}

static int _ws_srv_http_header_value_cb(llhttp_t* parser,
                                        const char* at, size_t len) {
    xylem_ws_conn_t* conn = parser->data;

    /* Upgrade: websocket */
    if (conn->current_header_field_len == 7 &&
        ws_memeqi(conn->current_header_field, "Upgrade", 7)) {
        if (len == 9 && ws_memeqi(at, "websocket", 9)) {
            conn->got_upgrade = true;
        }
    }

    /* Connection: Upgrade */
    if (conn->current_header_field_len == 10 &&
        ws_memeqi(conn->current_header_field, "Connection", 10)) {
        if (len == 7 && ws_memeqi(at, "Upgrade", 7)) {
            conn->got_connection = true;
        }
    }

    /* Sec-WebSocket-Key */
    if (conn->current_header_field_len == 17 &&
        ws_memeqi(conn->current_header_field, "Sec-WebSocket-Key", 17)) {
        if (len < sizeof(conn->client_ws_key)) {
            memcpy(conn->client_ws_key, at, len);
            conn->client_ws_key[len] = '\0';
            conn->client_ws_key_len = len;
            conn->got_ws_key = true;
        }
    }

    /* Sec-WebSocket-Version */
    if (conn->current_header_field_len == 21 &&
        ws_memeqi(conn->current_header_field,
                  "Sec-WebSocket-Version", 21)) {
        conn->got_ws_version = true;
        if (len == 2 && at[0] == '1' && at[1] == '3') {
            conn->version_ok = true;
        }
    }

    return 0;
}

static int _ws_srv_http_headers_complete_cb(llhttp_t* parser) {
    xylem_ws_conn_t* conn = parser->data;
    conn->handshake_complete = true;
    /* Return 2 to skip body and trigger HPE_PAUSED_UPGRADE */
    return 2;
}

static void _ws_srv_handshake_timeout_cb(xylem_loop_t* loop,
                                         xylem_loop_timer_t* timer,
                                         void* ud) {
    (void)loop;
    (void)timer;
    xylem_ws_conn_t* conn = ud;

    /* Timeout: close without sending HTTP response */
    conn->vt->close_conn(conn->transport);
}

static void _ws_srv_process_handshake(xylem_ws_conn_t* conn,
                                      const void* data, size_t len) {
    llhttp_errno_t err = llhttp_execute(&conn->http_parser,
                                        (const char*)data, len);

    if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
        /* HTTP parse error */
        static const char resp_400[] =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        _ws_srv_send_http_error(conn, resp_400, sizeof(resp_400) - 1);
        return;
    }

    if (!conn->handshake_complete) {
        return;
    }

    /* Stop handshake timer */
    if (conn->handshake_timer) {
        xylem_loop_stop_timer(conn->handshake_timer);
    }

    /* Validate required headers */
    if (!conn->got_upgrade || !conn->got_connection || !conn->got_ws_key) {
        static const char resp_400[] =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        _ws_srv_send_http_error(conn, resp_400, sizeof(resp_400) - 1);
        return;
    }

    /* Validate Sec-WebSocket-Version */
    if (!conn->got_ws_version || !conn->version_ok) {
        static const char resp_426[] =
            "HTTP/1.1 426 Upgrade Required\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        _ws_srv_send_http_error(conn, resp_426, sizeof(resp_426) - 1);
        return;
    }

    /* Compute Sec-WebSocket-Accept */
    char accept_value[29];
    if (ws_handshake_compute_accept(conn->client_ws_key,
                                    accept_value,
                                    sizeof(accept_value)) != 0) {
        static const char resp_500[] =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        _ws_srv_send_http_error(conn, resp_500, sizeof(resp_500) - 1);
        return;
    }

    /* Build and send 101 Switching Protocols response */
    size_t resp_len = 0;
    char* resp = ws_handshake_build_response(accept_value, &resp_len);
    if (!resp) {
        conn->vt->close_conn(conn->transport);
        return;
    }

    conn->vt->send(conn->transport, resp, resp_len);
    free(resp);

    /* Handshake succeeded, connection is now OPEN */
    conn->state = XYLEM_WS_STATE_OPEN;

    if (conn->handler && conn->handler->on_accept) {
        conn->handler->on_accept(conn);
    }

    /* Process leftover data after the HTTP request */
    const char* parsed_end = llhttp_get_error_pos(&conn->http_parser);
    if (parsed_end && err == HPE_PAUSED_UPGRADE) {
        const char* input_start = (const char*)data;
        size_t consumed = (size_t)(parsed_end - input_start);
        if (consumed < len) {
            size_t leftover = len - consumed;
            if (ws_conn_recv_buf_grow(conn, conn->recv_len + leftover) == 0) {
                memcpy(conn->recv_buf + conn->recv_len,
                       parsed_end, leftover);
                conn->recv_len += leftover;
                ws_conn_process_recv(conn);
            }
        }
    }
}

static void _ws_srv_transport_read_cb(void* handle, void* ctx,
                                      void* data, size_t len) {
    (void)handle;
    xylem_ws_server_t* server = ctx;

    /*
     * The transport bridge passes the server as ctx for accepted
     * connections. Retrieve the actual WS connection from the
     * transport handle's userdata.
     */
    xylem_ws_conn_t* conn = server->vt->get_userdata(handle);
    if (!conn) {
        return;
    }

    if (conn->state == XYLEM_WS_STATE_CONNECTING) {
        _ws_srv_process_handshake(conn, data, len);
        return;
    }

    if (conn->state == XYLEM_WS_STATE_CLOSED) {
        return;
    }

    /* Append to recv buffer and process frames */
    if (ws_conn_recv_buf_grow(conn, conn->recv_len + len) != 0) {
        conn->vt->close_conn(conn->transport);
        return;
    }
    memcpy(conn->recv_buf + conn->recv_len, data, len);
    conn->recv_len += len;

    ws_conn_process_recv(conn);
}

static void _ws_srv_transport_close_cb(void* handle, void* ctx, int err) {
    (void)handle;
    (void)err;
    xylem_ws_server_t* server = ctx;

    xylem_ws_conn_t* conn = server->vt->get_userdata(handle);
    if (!conn) {
        return;
    }

    if (conn->state == XYLEM_WS_STATE_CLOSED) {
        return;
    }

    uint16_t    code       = conn->close_code;
    const char* reason     = NULL;
    size_t      reason_len = 0;

    if (!conn->close_received && !conn->close_sent) {
        code = 1006;
    } else if (conn->state == XYLEM_WS_STATE_CONNECTING) {
        code = 1006;
    }

    ws_conn_fire_close(conn, code, reason, reason_len);
}

static void _ws_srv_transport_accept_cb(void* handle, void* ctx) {
    xylem_ws_server_t* server = ctx;

    /* Create a new WS connection for the accepted transport handle */
    xylem_ws_conn_t* conn = ws_conn_create(server->loop, &server->opts);
    if (!conn) {
        /* Cannot allocate, close the raw transport connection */
        server->vt->close_conn(handle);
        return;
    }

    conn->vt        = server->vt;
    conn->transport  = handle;
    conn->handler    = server->handler;
    conn->is_client  = false;
    conn->state      = XYLEM_WS_STATE_CONNECTING;
    conn->server     = server;

    /* Initialize llhttp for HTTP request parsing */
    llhttp_settings_init(&conn->http_settings);
    conn->http_settings.on_header_field     = _ws_srv_http_header_field_cb;
    conn->http_settings.on_header_value     = _ws_srv_http_header_value_cb;
    conn->http_settings.on_headers_complete = _ws_srv_http_headers_complete_cb;
    llhttp_init(&conn->http_parser, HTTP_REQUEST, &conn->http_settings);
    conn->http_parser.data = conn;

    /*
     * Associate the conn as userdata on the transport handle so
     * subsequent read/close callbacks can retrieve it.
     */
    server->vt->set_userdata(handle, conn);

    /* Start handshake timeout */
    uint64_t hs_timeout = server->opts.handshake_timeout_ms;
    if (hs_timeout == 0) {
        hs_timeout = WS_DEFAULT_HANDSHAKE_TIMEOUT;
    }
    xylem_loop_start_timer(conn->handshake_timer,
                           _ws_srv_handshake_timeout_cb,
                           hs_timeout, 0);
}

xylem_ws_server_t* xylem_ws_listen(xylem_loop_t* loop,
                                   const xylem_ws_srv_cfg_t* cfg) {
    if (!loop || !cfg || !cfg->handler) {
        return NULL;
    }

    /* Select transport based on TLS configuration */
    const ws_transport_vt_t* vt;
    if (cfg->tls_cert && cfg->tls_key) {
        vt = ws_transport_tls();
        if (!vt) {
            return NULL;
        }
    } else {
        vt = ws_transport_tcp();
    }

    xylem_ws_server_t* server = calloc(1, sizeof(*server));
    if (!server) {
        return NULL;
    }

    server->loop    = loop;
    server->vt      = vt;
    server->handler = cfg->handler;
    server->running = true;

    /* Copy options with defaults */
    if (cfg->opts) {
        server->opts = *cfg->opts;
    }

    /* Set up transport callbacks */
    server->transport_cb.on_connect    = NULL;
    server->transport_cb.on_accept     = _ws_srv_transport_accept_cb;
    server->transport_cb.on_read       = _ws_srv_transport_read_cb;
    server->transport_cb.on_close      = _ws_srv_transport_close_cb;
    server->transport_cb.on_write_done = NULL;

    /* Resolve bind address */
    xylem_addr_t addr;
    if (xylem_addr_pton(cfg->host, cfg->port, &addr) != 0) {
        free(server);
        return NULL;
    }

    /* Start listening */
    void* listener = vt->listen(loop, &addr,
                                &server->transport_cb, server,
                                NULL, cfg->tls_cert, cfg->tls_key);
    if (!listener) {
        free(server);
        return NULL;
    }
    server->listener = listener;

    return server;
}

void xylem_ws_close_server(xylem_ws_server_t* server) {
    if (!server) {
        return;
    }

    server->running = false;

    /* Stop accepting new connections */
    if (server->listener) {
        server->vt->close_server(server->listener);
        server->listener = NULL;
    }

    free(server);
}
