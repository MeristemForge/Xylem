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

#include "xylem/net/xylem-ws.h"

#include "xylem/encoding/xylem-base64.h"
#include "xylem/sync/xylem-mutex.h"
#include "xylem/xylem-utils.h"

#include "net/http/http-utils.h"
#include "runtime/precond.h"
#include "ws-deflate.h"
#include "ws-frame.h"
#include "ws-handshake.h"
#include "ws-utf8.h"
#include "ws.h"

#include "net/http/llhttp/llhttp.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WS_IO_READ    1u
#define WS_IO_WRITE   2u
#define WS_IO_CLOSING 4u
#define WS_IO_CLOSED  8u

struct xylem_ws_conn_s {
    ws_transport_t    transport;
    bool              is_client;
    _Atomic uint32_t  io_state;
    _Atomic uint64_t  read_deadline;
    _Atomic uint64_t  write_deadline;
    xylem_mutex_t*    write_lock;
    uint8_t*          recv_buf;
    size_t            recv_len;
    size_t            recv_cap;
    uint8_t*          frag_buf;
    size_t            frag_len;
    size_t            frag_cap;
    uint8_t           frag_opcode;
    bool              frag_active;
    size_t            max_msg_size;
    size_t            fragment_threshold;
    uint64_t          close_timeout_ms;
    _Atomic bool      close_sent;
    _Atomic bool      close_received;
    void*             userdata;
    void*             msg_data;
    size_t            msg_len;
    xylem_ws_opcode_t msg_type;
    bool              msg_ready;
    ws_deflate_ctx_t  deflate_ctx;
    bool              deflate_requested;
    bool              deflate_context_takeover;
    bool              frag_compressed;
};

/* Only an Upgrade handshake: no HTTP server, routing or response writer. */
typedef struct {
    llhttp_t          parser;
    llhttp_settings_t settings;
    http_header_t*    headers;
    size_t            count, cap;
    uint64_t          deadline;
    char              name[256], value[4096];
    size_t            name_len, value_len;
} _ws_handshake_t;

static void _ws_opts_apply(xylem_ws_conn_t* conn, const xylem_ws_opts_t* opts) {
    if (opts) {
        conn->max_msg_size =
            opts->max_msg_size ? opts->max_msg_size : WS_DEFAULT_MAX_MSG_SIZE;
        conn->fragment_threshold = opts->fragment_threshold
                                       ? opts->fragment_threshold
                                       : WS_DEFAULT_FRAGMENT_THRESHOLD;
        conn->close_timeout_ms   = opts->close_timeout_ms
                                       ? opts->close_timeout_ms
                                       : WS_DEFAULT_CLOSE_TIMEOUT;
        if (opts->permessage_deflate) {
            conn->deflate_requested        = true;
            conn->deflate_context_takeover = opts->deflate_context_takeover;
        }
    } else {
        conn->max_msg_size       = WS_DEFAULT_MAX_MSG_SIZE;
        conn->fragment_threshold = WS_DEFAULT_FRAGMENT_THRESHOLD;
        conn->close_timeout_ms   = WS_DEFAULT_CLOSE_TIMEOUT;
    }
}

static xylem_ws_conn_t* _ws_conn_create(
    ws_transport_t         transport,
    bool                   is_client,
    const xylem_ws_opts_t* opts) {
    xylem_ws_conn_t* conn =
        (xylem_ws_conn_t*)calloc(1, sizeof(xylem_ws_conn_t));
    if (!conn) {
        return NULL;
    }

    conn->transport = transport;
    conn->is_client = is_client;
    conn->recv_buf  = (uint8_t*)malloc(WS_RECV_BUF_INIT);
    if (!conn->recv_buf) {
        free(conn);
        return NULL;
    }
    conn->write_lock = xylem_mutex_create();
    if (!conn->write_lock) {
        free(conn->recv_buf);
        free(conn);
        return NULL;
    }
    atomic_init(&conn->io_state, 0);
    atomic_init(&conn->read_deadline, 0);
    atomic_init(&conn->write_deadline, 0);
    atomic_init(&conn->close_sent, false);
    atomic_init(&conn->close_received, false);
    conn->recv_cap = WS_RECV_BUF_INIT;

    _ws_opts_apply(conn, opts);
    return conn;
}

static void _ws_message_clear(xylem_ws_conn_t* conn) {
    free(conn->msg_data);
    conn->msg_data  = NULL;
    conn->msg_len   = 0;
    conn->msg_ready = false;
}

static void _ws_conn_destroy(xylem_ws_conn_t* conn) {
    if (!conn) {
        return;
    }
    _ws_message_clear(conn);
    xylem_mutex_destroy(conn->write_lock);
    ws_deflate_cleanup(&conn->deflate_ctx);
    free(conn->recv_buf);
    free(conn->frag_buf);
    free(conn);
}

static void _ws_abort(xylem_ws_conn_t* conn) {
    if (!(atomic_fetch_or(&conn->io_state, WS_IO_CLOSED) & WS_IO_CLOSED)) {
        conn->transport.close(conn->transport.conn);
    }
}

static bool _ws_deadline_expired(uint64_t deadline) {
    return deadline &&
           xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= deadline;
}

static bool _ws_io_enter(xylem_ws_conn_t* conn, uint32_t direction) {
    uint32_t state = atomic_load(&conn->io_state);
    for (;;) {
        if (state & (direction | WS_IO_CLOSING | WS_IO_CLOSED)) {
            return false;
        }
        if (atomic_compare_exchange_weak(
                &conn->io_state,
                &state,
                state | direction)) {
            return true;
        }
    }
}

static int _ws_write_frame_impl(
    xylem_ws_conn_t* conn,
    bool             fin,
    uint8_t          opcode,
    const void*      data,
    size_t           len,
    bool             rsv1) {
    uint8_t hdr_buf[14];
    uint8_t mask_key[4] = {0};

    if (conn->is_client) {
        uint32_t r = (uint32_t)rand();
        memcpy(mask_key, &r, 4);
    }

    size_t hdr_len = ws_frame_encode_header(
        hdr_buf,
        fin,
        opcode,
        conn->is_client,
        mask_key,
        len);

    if (rsv1) {
        hdr_buf[0] |= 0x40;
    }

    int n = conn->transport.write(conn->transport.conn, hdr_buf, (int)hdr_len);
    if (n < 0) {
        return -1;
    }

    if (len > 0) {
        if (conn->is_client) {
            uint8_t* masked = (uint8_t*)malloc(len);
            if (!masked) {
                return -1;
            }
            memcpy(masked, data, len);
            ws_frame_apply_mask(masked, len, mask_key, 0);
            n = conn->transport.write(conn->transport.conn, masked, (int)len);
            free(masked);
        } else {
            n = conn->transport.write(conn->transport.conn, data, (int)len);
        }
        if (n < 0) {
            return -1;
        }
    }
    return 0;
}

static int _ws_write_frame(
    xylem_ws_conn_t* conn,
    bool             fin,
    uint8_t          opcode,
    const void*      data,
    size_t           len,
    bool             rsv1) {
    xylem_mutex_lock(conn->write_lock);
    int rc = -1;
    if (!(atomic_load(&conn->io_state) & WS_IO_CLOSED) &&
        !_ws_deadline_expired(atomic_load(&conn->write_deadline))) {
        rc = _ws_write_frame_impl(conn, fin, opcode, data, len, rsv1);
    }
    xylem_mutex_unlock(conn->write_lock);
    if (rc) {
        _ws_abort(conn);
    }
    return rc;
}

static int _ws_send_close_frame(
    xylem_ws_conn_t* conn,
    uint16_t         code,
    const char*      reason,
    size_t           reason_len) {
    uint8_t payload[125];
    int     plen = ws_frame_close_encode(
        code,
        reason,
        reason_len,
        payload,
        sizeof(payload));
    if (plen < 0) {
        plen = 0;
    }
    return _ws_write_frame(conn, true, 0x8, payload, (size_t)plen, false);
}

static int _ws_ensure_recv_buf(xylem_ws_conn_t* conn, size_t needed) {
    if (conn->recv_cap >= needed) {
        return 0;
    }
    size_t new_cap = conn->recv_cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    uint8_t* nb = (uint8_t*)realloc(conn->recv_buf, new_cap);
    if (!nb) {
        return -1;
    }
    conn->recv_buf = nb;
    conn->recv_cap = new_cap;
    return 0;
}

static int
_ws_frag_append(xylem_ws_conn_t* conn, const void* data, size_t len) {
    size_t needed = conn->frag_len + len;
    if (conn->max_msg_size && needed > conn->max_msg_size) {
        return -1;
    }
    if (needed > conn->frag_cap) {
        size_t new_cap = conn->frag_cap ? conn->frag_cap : 4096;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        uint8_t* nb = (uint8_t*)realloc(conn->frag_buf, new_cap);
        if (!nb) {
            return -1;
        }
        conn->frag_buf = nb;
        conn->frag_cap = new_cap;
    }
    memcpy(conn->frag_buf + conn->frag_len, data, len);
    conn->frag_len += len;
    return 0;
}

/**
 * Drop a fully-consumed frame from the front of recv_buf, sliding any
 * trailing (coalesced) frame data down to offset 0. Must be called only
 * after the frame's payload has been copied out, never before: the
 * payload aliases recv_buf and the slide would otherwise overwrite it
 * with the next frame's bytes.
 */
static void _ws_recv_consume(xylem_ws_conn_t* conn, size_t frame_total) {
    size_t remaining = conn->recv_len - frame_total;
    if (remaining > 0) {
        memmove(conn->recv_buf, conn->recv_buf + frame_total, remaining);
    }
    conn->recv_len = remaining;
}

static int
_ws_handshake_header_field_cb(llhttp_t* p, const char* at, size_t n) {
    _ws_handshake_t* h = p->data;
    if (n >= sizeof(h->name) - h->name_len) {
        return -1;
    }
    memcpy(h->name + h->name_len, at, n);
    h->name_len += n;
    return 0;
}

static int
_ws_handshake_header_value_cb(llhttp_t* p, const char* at, size_t n) {
    _ws_handshake_t* h = p->data;
    if (n >= sizeof(h->value) - h->value_len) {
        return -1;
    }
    memcpy(h->value + h->value_len, at, n);
    h->value_len += n;
    return 0;
}

static int _ws_handshake_header_complete_cb(llhttp_t* p) {
    _ws_handshake_t* h     = p->data;
    h->name[h->name_len]   = 0;
    h->value[h->value_len] = 0;
    if (h->count >= 64 || http_header_find(h->headers, h->count, h->name)) {
        return -1;
    }
    int rc = http_header_add(
        &h->headers,
        &h->count,
        &h->cap,
        h->name,
        h->name_len,
        h->value,
        h->value_len);
    h->name_len = h->value_len = 0;
    return rc;
}

static int _ws_handshake_headers_complete_cb(llhttp_t* p) {
    (void)p;
    return 2;
}

static bool _ws_handshake_has_token(const char* value, const char* token) {
    if (!value) {
        return false;
    }
    size_t wanted = strlen(token);
    while (*value) {
        while (*value == ' ' || *value == '\t' || *value == ',') {
            ++value;
        }
        const char* end = strchr(value, ',');
        if (!end) {
            end = value + strlen(value);
        }
        const char* trim = end;
        while (trim > value && (trim[-1] == ' ' || trim[-1] == '\t')) {
            --trim;
        }
        if ((size_t)(trim - value) == wanted) {
            size_t i = 0;
            for (; i < wanted; ++i) {
                if (http_lower_table[(uint8_t)value[i]] !=
                    http_lower_table[(uint8_t)token[i]]) {
                    break;
                }
            }
            if (i == wanted) {
                return true;
            }
        }
        value = *end ? end + 1 : end;
    }
    return false;
}

static const char*
_ws_handshake_get_header(_ws_handshake_t* h, const char* name) {
    return http_header_find(h->headers, h->count, name);
}

static int _ws_handshake_read(
    ws_transport_t*        t,
    bool                   server,
    _ws_handshake_t*       h,
    char                   buf[16384],
    size_t*                used,
    size_t*                total,
    const xylem_ws_opts_t* opts) {
    memset(h, 0, sizeof(*h));
    *total = 0;
    llhttp_settings_init(&h->settings);
    h->settings.on_header_field          = _ws_handshake_header_field_cb;
    h->settings.on_header_value          = _ws_handshake_header_value_cb;
    h->settings.on_header_value_complete = _ws_handshake_header_complete_cb;
    h->settings.on_headers_complete      = _ws_handshake_headers_complete_cb;
    llhttp_init(
        &h->parser,
        server ? HTTP_REQUEST : HTTP_RESPONSE,
        &h->settings);
    h->parser.data    = h;
    uint64_t timeout  = opts && opts->handshake_timeout_ms
                            ? opts->handshake_timeout_ms
                            : WS_DEFAULT_HANDSHAKE_TIMEOUT;
    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout;
    h->deadline       = deadline;
    t->set_rd_deadline(t->conn, deadline);
    t->set_wr_deadline(t->conn, deadline);
    while (*total < 16384) {
        int n = t->read(t->conn, buf + *total, (int)(16384 - *total));
        if (n <= 0) {
            return -1;
        }
        llhttp_errno_t err =
            llhttp_execute(&h->parser, buf + *total, (size_t)n);
        *total += (size_t)n;
        if (err == HPE_PAUSED_UPGRADE) {
            *used = (size_t)(llhttp_get_error_pos(&h->parser) - buf);
            return 0;
        }
        if (err != HPE_OK) {
            return -1;
        }
    }
    return -1;
}

static void _ws_handshake_deinit(_ws_handshake_t* h) {
    http_headers_free(h->headers, h->count);
}

static xylem_ws_conn_t* _ws_handshake_create_conn(
    ws_transport_t         t,
    bool                   client,
    const xylem_ws_opts_t* opts,
    const char*            buf,
    size_t                 used,
    size_t                 total) {
    xylem_ws_conn_t* c = _ws_conn_create(t, client, opts);
    if (!c) {
        return NULL;
    }
    size_t n = total - used;
    if (n && _ws_ensure_recv_buf(c, n)) {
        _ws_conn_destroy(c);
        return NULL;
    }
    if (n) {
        memcpy(c->recv_buf, buf + used, n);
    }
    c->recv_len = n;
    return c;
}

static xylem_ws_conn_t*
_ws_accept_handshake(ws_transport_t t, const xylem_ws_opts_t* opts) {
    _ws_handshake_t  h;
    char             buf[16384];
    size_t           used = 0, total = 0;
    xylem_ws_conn_t* c = NULL;
    if (_ws_handshake_read(&t, true, &h, buf, &used, &total, opts)) {
        goto fail;
    }
    const char* key     = _ws_handshake_get_header(&h, "Sec-WebSocket-Key");
    const char* version = _ws_handshake_get_header(&h, "Sec-WebSocket-Version");
    const char* length  = _ws_handshake_get_header(&h, "Content-Length");
    uint8_t     decoded[32];
    if (h.parser.method != HTTP_GET || h.parser.http_major != 1 ||
        h.parser.http_minor != 1 || !_ws_handshake_get_header(&h, "Host") ||
        !_ws_handshake_has_token(
            _ws_handshake_get_header(&h, "Upgrade"),
            "websocket") ||
        !_ws_handshake_has_token(
            _ws_handshake_get_header(&h, "Connection"),
            "upgrade") ||
        !version || strcmp(version, "13") || !key || strlen(key) != 24 ||
        xylem_base64_decode_std(
            (const uint8_t*)key,
            24,
            decoded,
            sizeof(decoded)) != 16 ||
        _ws_handshake_get_header(&h, "Transfer-Encoding") ||
        (length && strcmp(length, "0"))) {
        goto fail;
    }
    char accept[29], extensions[160] = "", ext[128];
    if (ws_handshake_compute_accept(key, accept, sizeof(accept))) {
        goto fail;
    }
    ws_deflate_offer_t offer   = {0};
    bool               deflate = false;
    if (opts && opts->permessage_deflate) {
        const char* value =
            _ws_handshake_get_header(&h, "Sec-WebSocket-Extensions");
        if (value && !ws_deflate_parse_offer(value, &offer)) {
            if (!opts->deflate_context_takeover) {
                offer.server_no_context_takeover = true;
                offer.client_no_context_takeover = true;
            }
            if (!ws_deflate_build_server_accept(&offer, ext, sizeof(ext))) {
                snprintf(
                    extensions,
                    sizeof(extensions),
                    "Sec-WebSocket-Extensions: %s\r\n",
                    ext);
                deflate = true;
            }
        }
    }
    c = _ws_handshake_create_conn(t, false, opts, buf, used, total);
    if (!c) {
        goto fail;
    }
    if (deflate &&
        ws_deflate_init(&c->deflate_ctx, offer.server_no_context_takeover)) {
        goto fail;
    }
    char response[512];
    int  n = snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
        "%s\r\n%s\r\n",
        accept,
        extensions);
    if (xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) >= h.deadline) {
        goto fail;
    }
    if (n < 0 || n >= sizeof(response) || t.write(t.conn, response, n)) {
        goto fail;
    }
    t.set_rd_deadline(t.conn, 0);
    t.set_wr_deadline(t.conn, 0);
    _ws_handshake_deinit(&h);
    return c;
fail:
    if (c) {
        _ws_conn_destroy(c);
    }
    _ws_handshake_deinit(&h);
    return NULL;
}

static int _ws_send_impl(
    xylem_ws_conn_t* conn,
    uint8_t          opcode,
    const void*      data,
    size_t           len) {
    if (!conn || conn->close_sent || conn->close_received) {
        return -1;
    }
    if (opcode != 0x1 && opcode != 0x2) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_send");

    const uint8_t* p           = (const uint8_t*)data;
    size_t         send_len    = len;
    void*          compressed  = NULL;
    bool           use_deflate = conn->deflate_ctx.active;

    if (use_deflate) {
        size_t comp_len = 0;
        if (ws_deflate_compress(
                &conn->deflate_ctx,
                data,
                len,
                &compressed,
                &comp_len) != 0) {
            return -1;
        }
        p        = (const uint8_t*)compressed;
        send_len = comp_len;
    }

    size_t threshold = conn->fragment_threshold;
    int    result    = 0;

    if (send_len <= threshold) {
        result = _ws_write_frame(
            conn,
            true,
            (uint8_t)opcode,
            p,
            send_len,
            use_deflate);
    } else {
        size_t offset = 0;
        bool   first  = true;
        while (offset < send_len) {
            size_t chunk = send_len - offset;
            if (chunk > threshold) {
                chunk = threshold;
            }
            bool    fin = (offset + chunk >= send_len);
            uint8_t op  = first ? (uint8_t)opcode : 0x0;
            /* RSV1 only on the first frame. */
            bool rsv1 = first && use_deflate;

            if (_ws_write_frame(conn, fin, op, p + offset, chunk, rsv1) != 0) {
                result = -1;
                break;
            }

            offset += chunk;
            first = false;
        }
    }

    free(compressed);
    return result;
}

static int _ws_ping_impl(xylem_ws_conn_t* conn, const void* data, size_t len) {
    if (!conn || conn->close_sent || conn->close_received) {
        return -1;
    }
    if (len > 125) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_ping");

    return _ws_write_frame(conn, true, 0x9, data, len, false);
}

static int _ws_recv_impl(xylem_ws_conn_t* conn) {
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_recv");

    if (conn->close_received) {
        return -1;
    }

    for (;;) {
        if ((atomic_load(&conn->io_state) & WS_IO_CLOSED) ||
            _ws_deadline_expired(atomic_load(&conn->read_deadline))) {
            return -1;
        }
        while (conn->recv_len > 0) {
            if (_ws_deadline_expired(atomic_load(&conn->read_deadline))) {
                return -1;
            }
            ws_frame_header_t fh;
            int               rc =
                ws_frame_decode_header(conn->recv_buf, conn->recv_len, &fh);
            if (rc == -1) {
                break;
            }
            if (rc == -2) {
                return -1;
            }

            /* RSV1 is only valid when permessage-deflate is negotiated. */
            if (fh.rsv1 && !conn->deflate_ctx.active) {
                return -1;
            }

            size_t frame_total = fh.header_size + (size_t)fh.payload_len;
            if (conn->recv_len < frame_total) {
                break;
            }

            uint8_t* payload = conn->recv_buf + fh.header_size;
            if (fh.masked) {
                ws_frame_apply_mask(
                    payload,
                    (size_t)fh.payload_len,
                    fh.mask_key,
                    0);
            }

            /**
             * `payload` aliases recv_buf, so the buffer must NOT be
             * compacted until the payload has been fully consumed below.
             * Compacting here lets a coalesced next frame (still masked)
             * overwrite this frame's payload before it is copied out,
             * corrupting fragmented / multi-frame reads. The compaction is
             * deferred to the loop tail, after the payload is consumed.
             */
            bool message_ready = false;

            if (fh.opcode == 0x8) { /* Close */
                uint16_t    code;
                const char* reason;
                size_t      rlen;
                ws_frame_close_decode(
                    payload,
                    (size_t)fh.payload_len,
                    &code,
                    &reason,
                    &rlen);
                conn->close_received = true;
                if (!conn->close_sent) {
                    _ws_send_close_frame(conn, code, reason, rlen);
                    conn->close_sent = true;
                }
                _ws_recv_consume(conn, frame_total);
                return -1;
            } else if (fh.opcode == 0x9) { /* Ping -> auto-pong */
                if (_ws_write_frame(
                        conn,
                        true,
                        0xA,
                        payload,
                        (size_t)fh.payload_len,
                        false)) {
                    return -1;
                }
            } else if (fh.opcode == 0xA) { /* Pong -> discard */
                /* noop */
            } else if (fh.opcode == 0x0) { /* Continuation */
                if (!conn->frag_active) {
                    return -1;
                }
                if (_ws_frag_append(conn, payload, (size_t)fh.payload_len) !=
                    0) {
                    return -1;
                }
                if (fh.fin) {
                    void*  msg_data = conn->frag_buf;
                    size_t msg_len  = conn->frag_len;

                    /* Decompress reassembled fragments if compressed. */
                    if (conn->frag_compressed && conn->deflate_ctx.active) {
                        void*  dec     = NULL;
                        size_t dec_len = 0;
                        if (ws_deflate_decompress(
                                &conn->deflate_ctx,
                                conn->frag_buf,
                                conn->frag_len,
                                &dec,
                                &dec_len,
                                conn->max_msg_size) != 0) {
                            return -1;
                        }
                        free(conn->frag_buf);
                        conn->frag_buf = NULL;
                        conn->frag_cap = 0;
                        msg_data       = dec;
                        msg_len        = dec_len;
                    }

                    if (conn->frag_opcode == 0x1) {
                        if (ws_utf8_validate(msg_data, msg_len) != 0) {
                            free(msg_data);
                            conn->frag_buf = NULL;
                            conn->frag_cap = 0;
                            return -1;
                        }
                    }
                    conn->msg_type = conn->frag_opcode == 0x1 ? XYLEM_WS_TEXT
                                                              : XYLEM_WS_BINARY;
                    conn->msg_data = msg_data;
                    conn->msg_len  = msg_len;
                    conn->frag_buf = NULL;
                    conn->frag_len = 0;
                    conn->frag_cap = 0;
                    conn->frag_active     = false;
                    conn->frag_compressed = false;
                    message_ready         = true;
                }
            } else { /* Text or Binary */
                if (conn->frag_active) {
                    return -1;
                }
                if (fh.fin) {
                    if (conn->max_msg_size &&
                        fh.payload_len > conn->max_msg_size) {
                        return -1;
                    }

                    void*  msg_data;
                    size_t msg_len;

                    if (fh.rsv1 && conn->deflate_ctx.active) {
                        /* Decompress single-frame message. */
                        void*  dec     = NULL;
                        size_t dec_len = 0;
                        if (ws_deflate_decompress(
                                &conn->deflate_ctx,
                                payload,
                                (size_t)fh.payload_len,
                                &dec,
                                &dec_len,
                                conn->max_msg_size) != 0) {
                            return -1;
                        }
                        msg_data = dec;
                        msg_len  = dec_len;
                    } else {
                        void* copy = (void*)malloc(
                            fh.payload_len ? (size_t)fh.payload_len : 1);
                        if (!copy) {
                            return -1;
                        }
                        if (fh.payload_len) {
                            memcpy(copy, payload, (size_t)fh.payload_len);
                        }
                        msg_data = copy;
                        msg_len  = (size_t)fh.payload_len;
                    }

                    if (fh.opcode == 0x1) {
                        if (ws_utf8_validate(msg_data, msg_len) != 0) {
                            free(msg_data);
                            return -1;
                        }
                    }
                    conn->msg_type =
                        fh.opcode == 0x1 ? XYLEM_WS_TEXT : XYLEM_WS_BINARY;
                    conn->msg_data = msg_data;
                    conn->msg_len  = msg_len;
                    message_ready  = true;
                } else {
                    conn->frag_active     = true;
                    conn->frag_opcode     = fh.opcode;
                    conn->frag_compressed = fh.rsv1;
                    conn->frag_len        = 0;
                    if (_ws_frag_append(
                            conn,
                            payload,
                            (size_t)fh.payload_len) != 0) {
                        return -1;
                    }
                }
            }

            /* Payload consumed: now safe to drop the frame from recv_buf. */
            _ws_recv_consume(conn, frame_total);

            if (message_ready) {
                conn->msg_ready = true;
                return 0;
            }
        }

        if (_ws_ensure_recv_buf(conn, conn->recv_len + 4096) != 0) {
            return -1;
        }
        int n = conn->transport.read(
            conn->transport.conn,
            conn->recv_buf + conn->recv_len,
            (int)(conn->recv_cap - conn->recv_len));
        if (n <= 0) {
            return -1;
        }
        conn->recv_len += (size_t)n;
    }
}

int ws_send(
    xylem_ws_conn_t*  conn,
    const void*       data,
    int               len,
    xylem_ws_opcode_t type) {
    if (!conn || len < 0 || (!data && len) ||
        (type != XYLEM_WS_TEXT && type != XYLEM_WS_BINARY)) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_send");
    if (!_ws_io_enter(conn, WS_IO_WRITE)) {
        return -1;
    }
    uint8_t opcode = type == XYLEM_WS_TEXT ? 0x1 : 0x2;
    int     rc     = _ws_send_impl(conn, opcode, data, (size_t)len);
    if (!rc && _ws_deadline_expired(atomic_load(&conn->write_deadline))) {
        rc = -1;
    }
    if (rc) {
        _ws_abort(conn);
    }
    atomic_fetch_and(&conn->io_state, ~WS_IO_WRITE);
    return rc;
}

int ws_ping(xylem_ws_conn_t* conn, const void* data, size_t len) {
    if (!conn || (!data && len) || len > 125) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_ping");
    if (!_ws_io_enter(conn, WS_IO_WRITE)) {
        return -1;
    }
    int rc = _ws_ping_impl(conn, data, len);
    if (rc) {
        _ws_abort(conn);
    }
    atomic_fetch_and(&conn->io_state, ~WS_IO_WRITE);
    return rc;
}

int ws_recv(
    xylem_ws_conn_t*   conn,
    void*              buf,
    int                len,
    xylem_ws_opcode_t* type) {
    if (!conn || len < 0 || (!buf && len)) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_recv");
    if (!_ws_io_enter(conn, WS_IO_READ)) {
        return -1;
    }
    int rc = 0;
    if (!conn->msg_ready) {
        rc = _ws_recv_impl(conn);
    }
    if (!rc && ((atomic_load(&conn->io_state) & WS_IO_CLOSED) ||
                _ws_deadline_expired(atomic_load(&conn->read_deadline)) ||
                conn->msg_len > INT_MAX)) {
        rc = -1;
    }
    if (rc) {
        _ws_message_clear(conn);
        _ws_abort(conn);
    } else if (conn->msg_len > (size_t)len) {
        /* Keep the whole message, including its type, for the next call. */
        rc = -1;
    } else {
        if (conn->msg_len) {
            memcpy(buf, conn->msg_data, conn->msg_len);
        }
        if (type) {
            *type = conn->msg_type;
        }
        rc = (int)conn->msg_len;
        _ws_message_clear(conn);
    }
    atomic_fetch_and(&conn->io_state, ~WS_IO_READ);
    return rc;
}

int ws_close(
    xylem_ws_conn_t* conn,
    uint16_t         code,
    const char*      reason,
    size_t           reason_len) {
    if (!conn) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_close");
    uint32_t state = atomic_fetch_or(&conn->io_state, WS_IO_CLOSING);
    if (state & (WS_IO_CLOSING | WS_IO_CLOSED)) {
        return 0;
    }
    if (state & (WS_IO_READ | WS_IO_WRITE)) {
        _ws_abort(conn);
        return 0;
    }
    uint64_t deadline =
        xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + conn->close_timeout_ms;
    uint64_t rd = atomic_load(&conn->read_deadline);
    uint64_t wr = atomic_load(&conn->write_deadline);
    if (rd && rd < deadline) {
        deadline = rd;
    }
    if (wr && wr < deadline) {
        deadline = wr;
    }
    atomic_store(&conn->read_deadline, deadline);
    atomic_store(&conn->write_deadline, deadline);
    conn->transport.set_rd_deadline(conn->transport.conn, deadline);
    conn->transport.set_wr_deadline(conn->transport.conn, deadline);

    _ws_message_clear(conn);
    int rc = 0;
    if (!conn->close_sent) {
        if (code && ws_frame_close_validate_send(code)) {
            code = 1000;
        }
        rc = _ws_send_close_frame(conn, code ? code : 1000, reason, reason_len);
        conn->close_sent = true;
    }
    while (!rc && !conn->close_received) {
        rc = _ws_recv_impl(conn);
        _ws_message_clear(conn);
    }
    if (conn->close_received) {
        rc = 0;
    }
    _ws_abort(conn);
    return rc;
}

void ws_destroy(xylem_ws_conn_t* conn) {
    if (!conn) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_destroy");
    _ws_abort(conn);
    conn->transport.destroy(conn->transport.conn);
    _ws_conn_destroy(conn);
}

void ws_set_deadline(xylem_ws_conn_t* conn, uint64_t deadline_ms, bool write) {
    if (!conn) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_set_deadline");
    if (atomic_load(&conn->io_state) & WS_IO_CLOSED) {
        return;
    }
    if (write) {
        atomic_store(&conn->write_deadline, deadline_ms);
        conn->transport.set_wr_deadline(conn->transport.conn, deadline_ms);
    } else {
        atomic_store(&conn->read_deadline, deadline_ms);
        conn->transport.set_rd_deadline(conn->transport.conn, deadline_ms);
    }
}

int ws_addr(
    xylem_ws_conn_t* conn,
    char*            host,
    size_t           host_len,
    uint16_t*        port,
    bool             remote) {
    if (!conn) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_addr");
    if (atomic_load(&conn->io_state) & WS_IO_CLOSED) {
        return -1;
    }
    if (remote) {
        return conn->transport.remote_addr ? conn->transport.remote_addr(
                                                 conn->transport.conn,
                                                 host,
                                                 host_len,
                                                 port)
                                           : -1;
    }
    return conn->transport.local_addr
               ? conn->transport
                     .local_addr(conn->transport.conn, host, host_len, port)
               : -1;
}

xylem_ws_conn_t*
ws_accept_impl(ws_transport_t transport, const xylem_ws_opts_t* opts) {
    return _ws_accept_handshake(transport, opts);
}

xylem_ws_conn_t* ws_dial_impl(
    ws_transport_t         t,
    const char*            host,
    uint16_t               port,
    const char*            path,
    const xylem_ws_opts_t* opts) {
    char        key[25], offer[128];
    const char* extension = NULL;
    if (ws_handshake_gen_key(key, sizeof(key))) {
        t.destroy(t.conn);
        return NULL;
    }
    if (opts && opts->permessage_deflate &&
        !ws_deflate_build_client_offer(
            opts->deflate_context_takeover,
            offer,
            sizeof(offer))) {
        extension = offer;
    }
    size_t len;
    char*  request =
        ws_handshake_build_request_ext(host, port, path, key, extension, &len);
    if (!request) {
        t.destroy(t.conn);
        return NULL;
    }
    uint64_t timeout  = opts && opts->handshake_timeout_ms
                            ? opts->handshake_timeout_ms
                            : WS_DEFAULT_HANDSHAKE_TIMEOUT;
    uint64_t deadline = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC) + timeout;
    t.set_wr_deadline(t.conn, deadline);
    int rc = t.write(t.conn, request, (int)len);
    free(request);
    if (rc) {
        t.destroy(t.conn);
        return NULL;
    }
    _ws_handshake_t  h;
    char             buf[16384];
    size_t           used = 0, total = 0;
    xylem_ws_conn_t* c         = NULL;
    xylem_ws_opts_t  remaining = opts ? *opts : (xylem_ws_opts_t){0};
    uint64_t         now       = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
    if (now >= deadline) {
        t.destroy(t.conn);
        return NULL;
    }
    remaining.handshake_timeout_ms = deadline - now;
    if (_ws_handshake_read(&t, false, &h, buf, &used, &total, &remaining)) {
        goto fail;
    }
    const char* accept = _ws_handshake_get_header(&h, "Sec-WebSocket-Accept");
    if (h.parser.status_code != 101 || h.parser.http_major != 1 ||
        h.parser.http_minor != 1 ||
        !_ws_handshake_has_token(
            _ws_handshake_get_header(&h, "Upgrade"),
            "websocket") ||
        !_ws_handshake_has_token(
            _ws_handshake_get_header(&h, "Connection"),
            "upgrade") ||
        !accept || ws_handshake_validate_accept(key, accept)) {
        goto fail;
    }
    const char* value =
        _ws_handshake_get_header(&h, "Sec-WebSocket-Extensions");
    ws_deflate_offer_t server = {0};
    if (value && (!extension || ws_deflate_parse_offer(value, &server))) {
        goto fail;
    }
    c = _ws_handshake_create_conn(t, true, opts, buf, used, total);
    if (!c) {
        goto fail;
    }
    if (value && ws_deflate_init(
                     &c->deflate_ctx,
                     server.server_no_context_takeover ||
                         !opts->deflate_context_takeover)) {
        goto fail;
    }
    t.set_rd_deadline(t.conn, 0);
    t.set_wr_deadline(t.conn, 0);
    _ws_handshake_deinit(&h);
    return c;
fail:
    if (c) {
        _ws_conn_destroy(c);
    }
    _ws_handshake_deinit(&h);
    t.destroy(t.conn);
    return NULL;
}

void* ws_get_userdata(xylem_ws_conn_t* conn) {
    if (!conn) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_get_userdata");

    return conn->userdata;
}

void ws_set_userdata(xylem_ws_conn_t* conn, void* ud) {
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_set_userdata");

    if (conn) {
        conn->userdata = ud;
    }
}
