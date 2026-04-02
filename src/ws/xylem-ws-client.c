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

#include "xylem/ws/xylem-ws-client.h"
#include "xylem/ws/xylem-ws-common.h"
#include "xylem/xylem-addr.h"
#include "xylem/xylem-utils.h"

#include "ws-common.h"
#include "ws-frame.h"
#include "ws-handshake.h"
#include "ws-transport.h"

#include "llhttp.h"

#include <stdlib.h>
#include <string.h>

static int _ws_http_header_field_cb(llhttp_t* parser,
                                    const char* at, size_t len) {
    xylem_ws_conn_t* conn = parser->data;
    conn->parsing_accept_header = false;

    /* Check for Sec-WebSocket-Accept header */
    if (len == 20 && ws_memeqi(at, "Sec-WebSocket-Accept", 20)) {
        conn->parsing_accept_header = true;
    }

    conn->current_header_field = at;
    conn->current_header_field_len = len;
    return 0;
}

static int _ws_http_header_value_cb(llhttp_t* parser,
                                    const char* at, size_t len) {
    xylem_ws_conn_t* conn = parser->data;

    if (conn->current_header_field_len == 7 &&
        ws_memeqi(conn->current_header_field, "Upgrade", 7)) {
        if (len == 9 && ws_memeqi(at, "websocket", 9)) {
            conn->got_upgrade = true;
        }
    }

    if (conn->current_header_field_len == 10 &&
        ws_memeqi(conn->current_header_field, "Connection", 10)) {
        if (len == 7 && ws_memeqi(at, "Upgrade", 7)) {
            conn->got_connection = true;
        }
    }

    if (conn->parsing_accept_header) {
        if (len < sizeof(conn->accept_value)) {
            memcpy(conn->accept_value, at, len);
            conn->accept_value[len] = '\0';
            conn->accept_value_len = len;
        }
        conn->parsing_accept_header = false;
    }

    return 0;
}

static int _ws_http_headers_complete_cb(llhttp_t* parser) {
    xylem_ws_conn_t* conn = parser->data;
    conn->handshake_complete = true;
    /* Return 2 to tell llhttp to skip body and trigger upgrade */
    return 2;
}

static void _ws_handshake_timeout_cb(xylem_loop_t* loop,
                                     xylem_loop_timer_t* timer,
                                     void* ud) {
    (void)loop;
    (void)timer;
    xylem_ws_conn_t* conn = ud;

    /* Handshake timed out -- close transport */
    conn->vt->close_conn(conn->transport);
}

static void _ws_process_handshake(xylem_ws_conn_t* conn,
                                  const void* data, size_t len) {
    llhttp_errno_t err = llhttp_execute(&conn->http_parser,
                                        (const char*)data, len);

    if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
        /* HTTP parse error -- handshake failed */
        conn->vt->close_conn(conn->transport);
        return;
    }

    if (!conn->handshake_complete) {
        return;
    }

    /* Stop handshake timer */
    if (conn->handshake_timer) {
        xylem_loop_stop_timer(conn->handshake_timer);
    }

    /* Validate 101 status */
    int status = llhttp_get_status_code(&conn->http_parser);
    if (status != 101) {
        conn->vt->close_conn(conn->transport);
        return;
    }

    /* Validate required headers */
    if (!conn->got_upgrade || !conn->got_connection) {
        conn->vt->close_conn(conn->transport);
        return;
    }

    /* Validate Sec-WebSocket-Accept */
    if (ws_handshake_validate_accept(conn->handshake_key,
                                     conn->accept_value) != 0) {
        conn->vt->close_conn(conn->transport);
        return;
    }

    /* Handshake succeeded */
    conn->state = XYLEM_WS_STATE_OPEN;

    if (conn->handler && conn->handler->on_open) {
        conn->handler->on_open(conn);
    }

    /* Check if there's leftover data after the HTTP response */
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

static void _ws_transport_connect_cb(void* handle, void* ctx) {
    (void)handle;
    xylem_ws_conn_t* conn = ctx;

    /* TCP/TLS connected -- send HTTP Upgrade request */
    size_t req_len = 0;
    char* req = ws_handshake_build_request(conn->host, conn->port,
                                           conn->path, conn->handshake_key,
                                           &req_len);
    if (!req) {
        conn->vt->close_conn(conn->transport);
        return;
    }

    conn->vt->send(conn->transport, req, req_len);
    free(req);
}

static void _ws_transport_read_cb(void* handle, void* ctx,
                                  void* data, size_t len) {
    (void)handle;
    xylem_ws_conn_t* conn = ctx;

    if (conn->state == XYLEM_WS_STATE_CONNECTING) {
        _ws_process_handshake(conn, data, len);
        return;
    }

    if (conn->state == XYLEM_WS_STATE_CLOSED) {
        return;
    }

    /* Append to recv buffer and process frames */
    if (ws_conn_recv_buf_grow(conn, conn->recv_len + len) != 0) {
        ws_conn_protocol_error(conn, 1009);
        return;
    }
    memcpy(conn->recv_buf + conn->recv_len, data, len);
    conn->recv_len += len;

    ws_conn_process_recv(conn);
}

static void _ws_transport_close_cb(void* handle, void* ctx, int err,
                                   const char* errmsg) {
    (void)handle;
    (void)err;
    (void)errmsg;
    xylem_ws_conn_t* conn = ctx;

    if (conn->state == XYLEM_WS_STATE_CLOSED) {
        return;
    }

    uint16_t    code       = conn->close_code;
    const char* reason     = NULL;
    size_t      reason_len = 0;

    if (!conn->close_received && !conn->close_sent) {
        /* Abnormal closure -- no close frame exchanged */
        code = 1006;
    } else if (conn->state == XYLEM_WS_STATE_CONNECTING) {
        /* Handshake failed */
        code = 1006;
    }

    ws_conn_fire_close(conn, code, reason, reason_len);
}

/**
 * Parse a ws:// or wss:// URL into host, port, path components.
 * Returns 0 on success, -1 on error. Sets *use_tls accordingly.
 */
static int _ws_parse_url(const char* url, char** host, uint16_t* port,
                         char** path, bool* use_tls) {
    if (!url) {
        return -1;
    }

    const char* p = url;
    if (strncmp(p, "ws://", 5) == 0) {
        *use_tls = false;
        p += 5;
    } else if (strncmp(p, "wss://", 6) == 0) {
        *use_tls = true;
        p += 6;
    } else {
        return -1;
    }

    /* Find end of host (: or / or end of string) */
    const char* host_start = p;
    const char* host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/') {
        host_end++;
    }

    size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0) {
        return -1;
    }

    *host = malloc(host_len + 1);
    if (!*host) {
        return -1;
    }
    memcpy(*host, host_start, host_len);
    (*host)[host_len] = '\0';

    /* Parse port if present */
    p = host_end;
    if (*p == ':') {
        p++;
        long pval = strtol(p, (char**)&p, 10);
        if (pval <= 0 || pval > 65535) {
            free(*host);
            *host = NULL;
            return -1;
        }
        *port = (uint16_t)pval;
    } else {
        *port = *use_tls ? 443 : 80;
    }

    /* Parse path */
    if (*p == '/') {
        size_t path_len = strlen(p);
        *path = malloc(path_len + 1);
        if (!*path) {
            free(*host);
            *host = NULL;
            return -1;
        }
        memcpy(*path, p, path_len + 1);
    } else {
        *path = malloc(2);
        if (!*path) {
            free(*host);
            *host = NULL;
            return -1;
        }
        (*path)[0] = '/';
        (*path)[1] = '\0';
    }

    return 0;
}

xylem_ws_conn_t* xylem_ws_dial(xylem_loop_t* loop,
                               const char* url,
                               xylem_ws_handler_t* handler,
                               xylem_ws_opts_t* opts) {
    if (!loop || !url || !handler) {
        return NULL;
    }

    char*    host    = NULL;
    uint16_t port    = 0;
    char*    path    = NULL;
    bool     use_tls = false;

    if (_ws_parse_url(url, &host, &port, &path, &use_tls) != 0) {
        return NULL;
    }

    /* Select transport */
    const ws_transport_vt_t* vt;
    if (use_tls) {
        vt = ws_transport_tls();
        if (!vt) {
            free(host);
            free(path);
            return NULL;
        }
    } else {
        vt = ws_transport_tcp();
    }

    /* Allocate connection via shared factory */
    xylem_ws_conn_t* conn = ws_conn_create(loop, opts);
    if (!conn) {
        free(host);
        free(path);
        return NULL;
    }

    conn->vt        = vt;
    conn->handler   = handler;
    conn->state     = XYLEM_WS_STATE_CONNECTING;
    conn->is_client = true;
    conn->host      = host;
    conn->port      = port;
    conn->path      = path;

    uint64_t handshake_timeout = WS_DEFAULT_HANDSHAKE_TIMEOUT;
    if (opts && opts->handshake_timeout_ms > 0) {
        handshake_timeout = opts->handshake_timeout_ms;
    }

    /* Generate handshake key */
    if (ws_handshake_gen_key(conn->handshake_key,
                             sizeof(conn->handshake_key)) != 0) {
        ws_conn_destroy(conn);
        return NULL;
    }

    /* Initialize llhttp for response parsing */
    llhttp_settings_init(&conn->http_settings);
    conn->http_settings.on_header_field    = _ws_http_header_field_cb;
    conn->http_settings.on_header_value    = _ws_http_header_value_cb;
    conn->http_settings.on_headers_complete = _ws_http_headers_complete_cb;
    llhttp_init(&conn->http_parser, HTTP_RESPONSE, &conn->http_settings);
    conn->http_parser.data = conn;

    /* Set up transport callbacks */
    conn->transport_cb.on_connect    = _ws_transport_connect_cb;
    conn->transport_cb.on_read       = _ws_transport_read_cb;
    conn->transport_cb.on_close      = _ws_transport_close_cb;
    conn->transport_cb.on_accept     = NULL;
    conn->transport_cb.on_write_done = NULL;

    /* Resolve address and dial */
    xylem_addr_t addr;
    if (xylem_addr_pton(host, port, &addr) != 0) {
        ws_conn_destroy(conn);
        return NULL;
    }

    void* transport = vt->dial(loop, &addr, &conn->transport_cb, conn, NULL);
    if (!transport) {
        ws_conn_destroy(conn);
        return NULL;
    }
    conn->transport = transport;

    /* Start handshake timeout */
    xylem_loop_start_timer(conn->handshake_timer,
                           _ws_handshake_timeout_cb,
                           conn, handshake_timeout, 0);

    return conn;
}

int xylem_ws_send(xylem_ws_conn_t* conn,
                  xylem_ws_opcode_t opcode,
                  const void* data, size_t len) {
    if (!conn) {
        return -1;
    }

    /* Only allow sending in OPEN state */
    if (conn->state != XYLEM_WS_STATE_OPEN) {
        return -1;
    }

    /* Only text and binary opcodes allowed */
    if (opcode != XYLEM_WS_OPCODE_TEXT && opcode != XYLEM_WS_OPCODE_BINARY) {
        return -1;
    }

    const uint8_t* payload = (const uint8_t*)data;
    size_t threshold = conn->fragment_threshold;

    if (len <= threshold) {
        /* Single frame */
        return ws_conn_send_frame(conn, true, (uint8_t)opcode, payload, len);
    }

    /* Auto-fragmentation: split into multiple frames */
    size_t offset = 0;
    bool first = true;

    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > threshold) {
            chunk = threshold;
        }

        bool fin = (offset + chunk >= len);
        uint8_t op = first ? (uint8_t)opcode : 0x0;

        int rc = ws_conn_send_frame(conn, fin, op, payload + offset, chunk);
        if (rc != 0) {
            return -1;
        }

        offset += chunk;
        first = false;
    }

    return 0;
}

int xylem_ws_ping(xylem_ws_conn_t* conn,
                  const void* data, size_t len) {
    if (!conn) {
        return -1;
    }
    if (conn->state != XYLEM_WS_STATE_OPEN) {
        return -1;
    }
    /* Control frame payload must be <= 125 bytes */
    if (len > 125) {
        return -1;
    }
    return ws_conn_send_frame(conn, true, WS_OPCODE_PING, data, len);
}

int xylem_ws_close(xylem_ws_conn_t* conn,
                   uint16_t code, const char* reason, size_t reason_len) {
    if (!conn) {
        return -1;
    }

    /* Only allow close from OPEN state */
    if (conn->state != XYLEM_WS_STATE_OPEN) {
        return -1;
    }

    /* Validate status code */
    if (ws_frame_close_validate_send(code) != 0) {
        return -1;
    }

    conn->state = XYLEM_WS_STATE_CLOSING;
    conn->close_code = code;

    ws_conn_send_close_frame(conn, code, reason, reason_len);

    /* Start close timeout */
    xylem_loop_start_timer(conn->close_timer,
                           ws_conn_close_timeout_cb,
                           conn, conn->close_timeout_ms, 0);

    return 0;
}

const xylem_addr_t* xylem_ws_get_peer_addr(xylem_ws_conn_t* conn) {
    if (!conn || !conn->vt || !conn->vt->get_peer_addr) {
        return NULL;
    }
    return conn->vt->get_peer_addr(conn->transport);
}

void* xylem_ws_get_userdata(xylem_ws_conn_t* conn) {
    if (!conn) {
        return NULL;
    }
    return conn->userdata;
}

void xylem_ws_set_userdata(xylem_ws_conn_t* conn, void* ud) {
    if (!conn) {
        return;
    }
    conn->userdata = ud;
}
