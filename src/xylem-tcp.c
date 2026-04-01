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

#include "xylem/xylem-tcp.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-varint.h"
#include "xylem/xylem-queue.h"
#include "xylem/xylem-list.h"

#include "platform/platform-socket.h"
#include "xylem-loop-io.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TCP_DEFAULT_READ_BUF_SIZE 65536

typedef enum {
    TCP_STATE_CONNECTING,
    TCP_STATE_CONNECTED,
    TCP_STATE_CLOSING,
    TCP_STATE_CLOSED,
} _tcp_state_t;

typedef struct _tcp_write_req_s {
    xylem_queue_node_t node;
    void*              data;
    size_t             len;
    size_t             offset;
} _tcp_write_req_t;

/**
 * Dial-private state: only allocated for outbound (dialed) connections.
 * Holds connect/reconnect timers, peer address, and retry counter.
 * host/port_str are resolved once at dial time and reused on reconnect.
 */
typedef struct _tcp_dial_priv_s {
    xylem_loop_timer_t*   connect_timer;
    xylem_loop_timer_t*   reconnect_timer;
    xylem_addr_t          peer_addr;
    uint32_t              reconnect_count;
    xylem_tcp_conn_t*     conn;
    xylem_loop_timer_fn_t reconnect_cb;
    char                  host[INET6_ADDRSTRLEN];
    char                  port_str[8];
} _tcp_dial_priv_t;

struct xylem_tcp_conn_s {
    xylem_loop_t*         loop;
    xylem_loop_io_t*      io;
    platform_sock_t       fd;
    xylem_tcp_handler_t*  handler;
    xylem_tcp_opts_t      opts;
    _tcp_state_t          state;
    uint8_t*              read_buf;
    size_t                read_len;
    size_t                read_cap;
    xylem_queue_t         write_queue;
    xylem_loop_timer_t*   read_timer;
    xylem_loop_timer_t*   write_timer;
    xylem_loop_timer_t*   heartbeat_timer;
    _tcp_dial_priv_t*     dial;
    xylem_list_node_t     server_node;
    xylem_tcp_server_t*   server;
    xylem_addr_t          peer_addr;
    void*                 userdata;
};

struct xylem_tcp_server_s {
    xylem_loop_t*        loop;
    xylem_loop_io_t*     io;
    platform_sock_t      fd;
    xylem_tcp_handler_t* handler;
    xylem_tcp_opts_t     opts;
    xylem_list_t         connections;
    void*                userdata;
    bool                 closing;
};

/**
 * Extract one complete frame from the connection's read buffer.
 * Returns > 0 on success (bytes consumed), 0 if data insufficient,
 * < 0 on error. On success, *frame_out points into conn->read_buf
 * (zero-copy) and *frame_len_out is the payload length.
 * The pointer is valid until the next recv or compact.
 */
static ssize_t _tcp_extract_frame(xylem_tcp_conn_t* conn,
                                  void** frame_out,
                                  size_t* frame_len_out) {
    uint8_t* data  = conn->read_buf;
    size_t   avail = conn->read_len;

    if (avail == 0) {
        return 0;
    }

    switch (conn->opts.framing.type) {

    case XYLEM_TCP_FRAME_NONE: {
        *frame_out     = data;
        *frame_len_out = avail;
        return (ssize_t)avail;
    }

    case XYLEM_TCP_FRAME_FIXED: {
        size_t fsz = conn->opts.framing.fixed.frame_size;
        if (fsz == 0) {
            xylem_logw("tcp conn fd=%d frame_fixed: frame_size=0",
                       (int)conn->fd);
            return -1;
        }
        if (avail < fsz) {
            return 0;
        }

        *frame_out     = data;
        *frame_len_out = fsz;
        return (ssize_t)fsz;
    }

    case XYLEM_TCP_FRAME_LENGTH: {
        uint32_t hdr_sz  = conn->opts.framing.length.header_size;
        uint32_t len_off = conn->opts.framing.length.field_offset;
        uint32_t len_sz  = conn->opts.framing.length.field_size;
        int32_t  adj     = conn->opts.framing.length.adjustment;

        if (avail < hdr_sz) {
            return 0;
        }

        uint32_t effective_hdr = hdr_sz;
        uint64_t payload_len = 0;

        if (conn->opts.framing.length.coding == XYLEM_TCP_LENGTH_FIXEDINT) {
            if (len_sz == 0 || len_sz > 8) {
                xylem_logw("tcp conn fd=%d frame_length: invalid field_size=%u",
                           (int)conn->fd, len_sz);
                return -1;
            }
            if (avail < len_off + len_sz) {
                return 0;
            }

            if (conn->opts.framing.length.field_big_endian) {
                for (uint32_t i = 0; i < len_sz; i++) {
                    payload_len = (payload_len << 8) | data[len_off + i];
                }
            } else {
                for (uint32_t i = 0; i < len_sz; i++) {
                    payload_len |= (uint64_t)data[len_off + i] << (i * 8);
                }
            }
        } else {
            size_t pos = (size_t)len_off;
            if (!xylem_varint_decode(data, avail, &pos, &payload_len)) {
                if (avail < hdr_sz + 10) {
                    return 0;
                }
                return -1;
            }
            uint32_t varint_bytes = (uint32_t)(pos - len_off);
            effective_hdr = hdr_sz + varint_bytes - len_sz;
        }

        int64_t frame_size = (int64_t)effective_hdr + (int64_t)payload_len +
                             (int64_t)adj;
        if (frame_size <= 0) {
            xylem_logw("tcp conn fd=%d frame_length: frame_size=%" PRId64
                       " <= 0", (int)conn->fd, frame_size);
            return -1;
        }

        size_t total = (size_t)frame_size;
        if (avail < total) {
            return 0;
        }

        if (total <= effective_hdr) {
            *frame_out     = NULL;
            *frame_len_out = 0;
        } else {
            *frame_out     = data + effective_hdr;
            *frame_len_out = total - effective_hdr;
        }
        return (ssize_t)total;
    }

    case XYLEM_TCP_FRAME_DELIM: {
        const char* delim     = conn->opts.framing.delim.delim;
        size_t      delim_len = conn->opts.framing.delim.delim_len;
        if (!delim || delim_len == 0) {
            xylem_logw("tcp conn fd=%d frame_delim: delim is NULL or empty",
                       (int)conn->fd);
            return -1;
        }

        ssize_t found_at = -1;
        if (delim_len == 1) {
            const void* p = memchr(data, delim[0], avail);
            if (p) {
                found_at = (ssize_t)((const uint8_t*)p - data);
            }
        } else {
            for (size_t i = 0; i + delim_len <= avail; i++) {
                if (memcmp(data + i, delim, delim_len) == 0) {
                    found_at = (ssize_t)i;
                    break;
                }
            }
        }

        if (found_at < 0) {
            return 0;
        }

        size_t frame_len   = (size_t)found_at;
        size_t consume_len = frame_len + delim_len;

        *frame_out     = (frame_len > 0) ? data : NULL;
        *frame_len_out = frame_len;
        return (ssize_t)consume_len;
    }

    case XYLEM_TCP_FRAME_CUSTOM: {
        if (!conn->opts.framing.custom.parse) {
            xylem_logw("tcp conn fd=%d frame_custom: parse is NULL",
                       (int)conn->fd);
            return -1;
        }

        int rc = conn->opts.framing.custom.parse(data, avail);

        if (rc > 0) {
            if ((size_t)rc > avail) {
                xylem_logw("tcp conn fd=%d frame_custom: parse returned %d"
                           " > avail %zu", (int)conn->fd, rc, avail);
                return -1;
            }
            *frame_out     = data;
            *frame_len_out = (size_t)rc;
            return rc;
        }

        return rc;
    }

    default:
        return -1;
    }
}

static void _tcp_resolve_hostport(xylem_addr_t* addr,
                                  char* host, size_t host_len,
                                  char* port_str, size_t port_str_len) {
    uint16_t port = 0;
    xylem_addr_ntop(addr, host, host_len, &port);
    snprintf(port_str, port_str_len, "%u", port);
}

static void _tcp_read_timeout_cb(xylem_loop_t* loop,
                                 xylem_loop_timer_t* timer,
                                 void* ud) {
    (void)loop;
    (void)timer;
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)ud;

    xylem_logw("tcp conn fd=%d read timeout", (int)conn->fd);
    if (conn->handler && conn->handler->on_timeout) {
        conn->handler->on_timeout(conn, XYLEM_TCP_TIMEOUT_READ);
    }
}

static void _tcp_write_timeout_cb(xylem_loop_t* loop,
                                  xylem_loop_timer_t* timer,
                                  void* ud) {
    (void)loop;
    (void)timer;
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)ud;

    xylem_logw("tcp conn fd=%d write timeout", (int)conn->fd);
    if (conn->handler && conn->handler->on_timeout) {
        conn->handler->on_timeout(conn, XYLEM_TCP_TIMEOUT_WRITE);
    }
}

static void _tcp_destroy_conn(xylem_tcp_conn_t* conn, int err);
static void _tcp_start_reconnect_timer(xylem_tcp_conn_t* conn,
                                       xylem_loop_timer_fn_t cb);

static void _tcp_connect_timeout_cb(xylem_loop_t* loop,
                                    xylem_loop_timer_t* timer,
                                    void* ud) {
    (void)loop;
    (void)timer;
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)ud;

    xylem_logw("tcp conn fd=%d connect timeout", (int)conn->fd);
    if (conn->handler && conn->handler->on_timeout) {
        conn->handler->on_timeout(conn, XYLEM_TCP_TIMEOUT_CONNECT);
    }

    /* User may have closed the connection in on_timeout. */
    if (conn->state == TCP_STATE_CLOSED ||
        conn->state == TCP_STATE_CLOSING) {
        return;
    }

    /* Stop watching the stale socket and attempt reconnect or close. */
    xylem_loop_stop_io(conn->io);

    _tcp_dial_priv_t* dial = conn->dial;
    if (dial && conn->opts.reconnect_max > 0 &&
        dial->reconnect_count < conn->opts.reconnect_max) {
        _tcp_start_reconnect_timer(conn, dial->reconnect_cb);
    } else {
        _tcp_destroy_conn(conn, PLATFORM_SO_ERROR_ETIMEDOUT);
    }
}

static void _tcp_heartbeat_timeout_cb(xylem_loop_t* loop,
                                      xylem_loop_timer_t* timer,
                                      void* ud) {
    (void)loop;
    (void)timer;
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)ud;

    xylem_logw("tcp conn fd=%d heartbeat miss", (int)conn->fd);
    if (conn->handler && conn->handler->on_heartbeat_miss) {
        conn->handler->on_heartbeat_miss(conn);
    }
}

/* Post callback: free a connection after the current iteration. */
static void _tcp_conn_free_cb(xylem_loop_t* loop,
                              xylem_loop_post_t* req,
                              void* ud) {
    (void)loop;
    (void)req;
    free(ud);
}

static void _tcp_destroy_conn(xylem_tcp_conn_t* conn, int err) {
    conn->state = TCP_STATE_CLOSED;
    xylem_logd("tcp conn fd=%d destroy err=%d (%s)",
               (int)conn->fd, err,
               err ? platform_socket_tostring(err) : "ok");

    if (conn->server) {
        xylem_list_remove(&conn->server->connections, &conn->server_node);
        conn->server = NULL;
    }

    if (conn->dial) {
        xylem_loop_destroy_timer(conn->dial->connect_timer);
        conn->dial->connect_timer = NULL;
        xylem_loop_destroy_timer(conn->dial->reconnect_timer);
        conn->dial->reconnect_timer = NULL;
    }

    xylem_loop_destroy_timer(conn->read_timer);
    conn->read_timer = NULL;
    xylem_loop_destroy_timer(conn->write_timer);
    conn->write_timer = NULL;
    xylem_loop_destroy_timer(conn->heartbeat_timer);
    conn->heartbeat_timer = NULL;

    xylem_loop_destroy_io(conn->io);
    conn->io = NULL;
    platform_socket_close(conn->fd);

    free(conn->read_buf);
    conn->read_buf = NULL;

    if (conn->dial) {
        free(conn->dial);
        conn->dial = NULL;
    }

    if (conn->handler && conn->handler->on_close) {
        conn->handler->on_close(conn, err);
    }

    xylem_loop_post(conn->loop, _tcp_conn_free_cb, conn);
}

static void _tcp_close_conn(xylem_tcp_conn_t* conn, int err) {
    if (conn->state == TCP_STATE_CLOSED ||
        conn->state == TCP_STATE_CLOSING) {
        return;
    }

    xylem_logd("tcp conn fd=%d start_close err=%d (%s)",
               (int)conn->fd, err,
               err ? platform_socket_tostring(err) : "ok");
    conn->state = TCP_STATE_CLOSING;

    while (!xylem_queue_empty(&conn->write_queue)) {
        xylem_queue_node_t* node =
            xylem_queue_dequeue(&conn->write_queue);
        _tcp_write_req_t* req =
            xylem_queue_entry(node, _tcp_write_req_t, node);

        if (conn->handler && conn->handler->on_write_done) {
            conn->handler->on_write_done(conn, req->data, req->len, err);
        }

        free(req);
    }

    _tcp_destroy_conn(conn, err);
}

static void _tcp_conn_readable_cb(xylem_tcp_conn_t* conn) {
    for (;;) {
        size_t space = conn->read_cap - conn->read_len;
        if (space == 0) {
            xylem_logw("tcp conn fd=%d read buffer full, closing",
                       (int)conn->fd);
            _tcp_close_conn(conn, -1);
            return;
        }

        ssize_t nread = platform_socket_recv(
            conn->fd, conn->read_buf + conn->read_len, (int)space);

        if (nread == 0) {
            xylem_logi("tcp conn fd=%d peer closed", (int)conn->fd);
            _tcp_close_conn(conn, 0);
            return;
        }

        if (nread < 0) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                break;
            }
            xylem_logw("tcp conn fd=%d recv error=%d (%s)",
                       (int)conn->fd, err,
                       platform_socket_tostring(err));
            _tcp_close_conn(conn, err);
            return;
        }

        conn->read_len += (size_t)nread;
        xylem_logd("tcp conn fd=%d recv %zd bytes",
                   (int)conn->fd, nread);

        for (;;) {
            void*  frame_data = NULL;
            size_t frame_len  = 0;
            ssize_t rc = _tcp_extract_frame(conn, &frame_data, &frame_len);

            if (rc > 0) {
                if (conn->handler && conn->handler->on_read) {
                    conn->handler->on_read(conn, frame_data, frame_len);
                }

                /* Compact so next extract sees correct data. */
                conn->read_len -= (size_t)rc;
                if (conn->read_len > 0) {
                    memmove(conn->read_buf,
                            conn->read_buf + (size_t)rc,
                            conn->read_len);
                }

                if (conn->state == TCP_STATE_CLOSED ||
                    conn->state == TCP_STATE_CLOSING) {
                    return;
                }
            } else if (rc == 0) {
                break;
            } else {
                _tcp_close_conn(conn, -1);
                return;
            }
        }

        if ((size_t)nread < space) {
            break;
        }
    }

    if (conn->opts.heartbeat_ms > 0 && conn->heartbeat_timer) {
        xylem_loop_reset_timer(conn->heartbeat_timer,
                               conn->opts.heartbeat_ms);
    }

    if (conn->opts.read_timeout_ms > 0 && conn->read_timer) {
        xylem_loop_reset_timer(conn->read_timer,
                               conn->opts.read_timeout_ms);
    }
}

static void _tcp_conn_io_cb(xylem_loop_t* loop,
                            xylem_loop_io_t* io,
                            platform_poller_op_t revents,
                            void* ud);

static void _tcp_flush_writes(xylem_tcp_conn_t* conn) {
    while (!xylem_queue_empty(&conn->write_queue)) {
        xylem_queue_node_t* front =
            xylem_queue_front(&conn->write_queue);
        _tcp_write_req_t* req =
            xylem_queue_entry(front, _tcp_write_req_t, node);

        char*  ptr = (char*)req->data + req->offset;
        size_t rem = req->len - req->offset;

        ssize_t n = platform_socket_send(conn->fd, ptr, (int)rem);

        if (n < 0) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                return;
            }

            xylem_logw("tcp conn fd=%d send error=%d (%s)",
                       (int)conn->fd, err,
                       platform_socket_tostring(err));

            if (conn->state == TCP_STATE_CLOSING) {
                while (!xylem_queue_empty(&conn->write_queue)) {
                    xylem_queue_node_t* qn =
                        xylem_queue_dequeue(&conn->write_queue);
                    _tcp_write_req_t* wr =
                        xylem_queue_entry(qn, _tcp_write_req_t, node);
                    if (conn->handler && conn->handler->on_write_done) {
                        conn->handler->on_write_done(conn, wr->data,
                                                     wr->len, err);
                    }
                    free(wr);
                }
                _tcp_destroy_conn(conn, err);
            } else {
                _tcp_close_conn(conn, err);
            }
            return;
        }

        req->offset += (size_t)n;

        if (req->offset == req->len) {
            xylem_queue_dequeue(&conn->write_queue);
            xylem_logd("tcp conn fd=%d sent %zu bytes (complete)",
                       (int)conn->fd, req->len);

            if (conn->handler && conn->handler->on_write_done) {
                conn->handler->on_write_done(conn,
                    req->data, req->len, 0);
            }

            free(req);

            if (conn->state == TCP_STATE_CLOSED ||
                conn->state == TCP_STATE_CLOSING) {
                return;
            }

            if (conn->opts.write_timeout_ms > 0 && conn->write_timer &&
                !xylem_queue_empty(&conn->write_queue)) {
                xylem_loop_reset_timer(conn->write_timer,
                                       conn->opts.write_timeout_ms);
            }
        } else {
            xylem_logd("tcp conn fd=%d partial write %zd/%zu",
                       (int)conn->fd, n, rem);
            return;
        }
    }

    if (conn->opts.write_timeout_ms > 0 && conn->write_timer) {
        xylem_loop_stop_timer(conn->write_timer);
    }

    if (conn->state == TCP_STATE_CLOSING) {
        xylem_logd("tcp conn fd=%d write queue drained, shutting down",
                   (int)conn->fd);
        shutdown(conn->fd, PLATFORM_SHUT_WR);
        _tcp_destroy_conn(conn, 0);
    }
}

static void _tcp_conn_io_cb(xylem_loop_t* loop,
                            xylem_loop_io_t* io,
                            platform_poller_op_t revents,
                            void* ud) {
    (void)loop;
    (void)io;
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)ud;

    if (revents & PLATFORM_POLLER_RD_OP) {
        _tcp_conn_readable_cb(conn);
    }

    /* CLOSING is intentionally allowed through -- flush_writes needs to
     * drain the write queue before the connection is fully torn down. */
    if (conn->state == TCP_STATE_CLOSED) {
        return;
    }

    if (revents & PLATFORM_POLLER_WR_OP) {
        _tcp_flush_writes(conn);

        if (conn->state == TCP_STATE_CONNECTED &&
            xylem_queue_empty(&conn->write_queue)) {
            xylem_loop_start_io(conn->io, PLATFORM_POLLER_RD_OP,
                                _tcp_conn_io_cb, conn);
        }
    }
}

/**
 * Common setup for a newly connected socket: allocate read buffer,
 * start IO, start heartbeat/read timers. Does NOT call any handler
 * callback.
 */
static int _tcp_setup_conn(xylem_tcp_conn_t* conn) {
    conn->state    = TCP_STATE_CONNECTED;
    conn->read_buf = malloc(conn->opts.read_buf_size);
    if (!conn->read_buf) {
        return -1;
    }
    conn->read_len = 0;
    conn->read_cap = conn->opts.read_buf_size;

    if (xylem_loop_start_io(conn->io, PLATFORM_POLLER_RD_OP,
                            _tcp_conn_io_cb, conn) != 0) {
        free(conn->read_buf);
        conn->read_buf = NULL;
        return -1;
    }

    if (conn->opts.heartbeat_ms > 0) {
        if (!conn->heartbeat_timer) {
            conn->heartbeat_timer =
                xylem_loop_create_timer(conn->loop);
        }
        if (conn->heartbeat_timer) {
            xylem_loop_start_timer(conn->heartbeat_timer,
                                   _tcp_heartbeat_timeout_cb,
                                   conn, conn->opts.heartbeat_ms,
                                   conn->opts.heartbeat_ms);
        }
    }

    if (conn->opts.read_timeout_ms > 0) {
        if (!conn->read_timer) {
            conn->read_timer =
                xylem_loop_create_timer(conn->loop);
        }
        if (conn->read_timer) {
            xylem_loop_start_timer(conn->read_timer,
                                   _tcp_read_timeout_cb,
                                   conn, conn->opts.read_timeout_ms, 0);
        }
    }

    if (conn->opts.write_timeout_ms > 0) {
        if (!conn->write_timer) {
            conn->write_timer =
                xylem_loop_create_timer(conn->loop);
        }
    }

    return 0;
}

static void _tcp_conn_connected_cb(xylem_tcp_conn_t* conn) {
    if (_tcp_setup_conn(conn) != 0) {
        xylem_logw("tcp conn fd=%d setup failed", (int)conn->fd);
        _tcp_close_conn(conn, -1);
        return;
    }
    xylem_logi("tcp conn fd=%d connected", (int)conn->fd);

    if (conn->handler && conn->handler->on_connect) {
        conn->handler->on_connect(conn);
    }
}

static void _tcp_try_connect(xylem_loop_t* loop,
                             xylem_loop_io_t* io,
                             platform_poller_op_t revents,
                             void* ud);

/* Shared reconnect logic: check limit, compute backoff, start timer. */
static void _tcp_start_reconnect_timer(xylem_tcp_conn_t* conn,
                                       xylem_loop_timer_fn_t cb) {
    _tcp_dial_priv_t* dial = conn->dial;

    if (dial->reconnect_count >= conn->opts.reconnect_max) {
        xylem_logw("tcp conn fd=%d reconnect limit reached (%u)",
                   (int)conn->fd, conn->opts.reconnect_max);
        _tcp_close_conn(conn, PLATFORM_SO_ERROR_ETIMEDOUT);
        return;
    }

    uint64_t delay = 500ULL << (dial->reconnect_count < 16
                                ? dial->reconnect_count : 16);
    if (delay > 30000) {
        delay = 30000;
    }

    xylem_loop_start_timer(dial->reconnect_timer, cb, conn, delay, 0);
    xylem_logi("tcp conn fd=%d scheduling reconnect #%u in %" PRIu64 " ms",
               (int)conn->fd, dial->reconnect_count + 1,
               delay);
}

static void _tcp_try_connect(xylem_loop_t* loop,
                             xylem_loop_io_t* io,
                             platform_poller_op_t revents,
                             void* ud) {
    (void)loop;
    (void)io;
    (void)revents;
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)ud;
    _tcp_dial_priv_t* dial = conn->dial;

    int err    = 0;
    socklen_t errlen = sizeof(err);

    getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen);

    xylem_logd("tcp conn fd=%d connect result SO_ERROR=%d (%s)",
               (int)conn->fd, err,
               err ? platform_socket_tostring(err) : "ok");

    if (conn->opts.connect_timeout_ms > 0 && dial->connect_timer) {
        xylem_loop_stop_timer(dial->connect_timer);
    }

    if (err == 0) {
        _tcp_conn_connected_cb(conn);
    } else {
        if (conn->opts.reconnect_max > 0 &&
            dial->reconnect_count < conn->opts.reconnect_max) {
            _tcp_start_reconnect_timer(conn, dial->reconnect_cb);
        } else {
            _tcp_destroy_conn(conn, err);
        }
    }
}

static void _tcp_reconnect_timeout_cb(xylem_loop_t* loop,
                                      xylem_loop_timer_t* timer,
                                      void* ud) {
    (void)loop;
    (void)timer;
    xylem_tcp_conn_t* conn = (xylem_tcp_conn_t*)ud;
    _tcp_dial_priv_t* dial = conn->dial;

    xylem_loop_stop_io(conn->io);
    platform_socket_close(conn->fd);

    bool connected = false;
    platform_sock_t fd = platform_socket_dial(dial->host, dial->port_str,
                                              SOCK_STREAM,
                                              &connected, true);

    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_logw("tcp reconnect: socket creation failed, retrying");
        dial->reconnect_count++;
        _tcp_start_reconnect_timer(conn, _tcp_reconnect_timeout_cb);
        return;
    }

    conn->fd = fd;
    xylem_loop_destroy_io(conn->io);
    conn->io = xylem_loop_create_io(conn->loop, fd);
    conn->state = TCP_STATE_CONNECTING;
    dial->reconnect_count++;

    if (connected) {
        _tcp_conn_connected_cb(conn);
    } else {
        xylem_loop_start_io(conn->io, PLATFORM_POLLER_WR_OP,
                            _tcp_try_connect, conn);

        if (conn->opts.connect_timeout_ms > 0 && dial->connect_timer) {
            xylem_loop_start_timer(dial->connect_timer,
                                   _tcp_connect_timeout_cb,
                                   conn, conn->opts.connect_timeout_ms, 0);
        }
    }
}

static void _tcp_server_io_cb(xylem_loop_t* loop,
                              xylem_loop_io_t* io,
                              platform_poller_op_t revents,
                              void* ud) {
    (void)io;
    (void)revents;
    xylem_tcp_server_t* server = (xylem_tcp_server_t*)ud;

    for (;;) {
        platform_sock_t client_fd =
            platform_socket_accept(server->fd, true);

        if (client_fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                break;
            }
            xylem_logw("tcp server fd=%d accept error=%d (%s)",
                       (int)server->fd, err,
                       platform_socket_tostring(err));
            continue;
        }

        xylem_tcp_conn_t* conn = calloc(1, sizeof(*conn));
        if (!conn) {
            xylem_logw("tcp server fd=%d accept: conn alloc failed",
                       (int)server->fd);
            platform_socket_close(client_fd);
            continue;
        }

        conn->loop    = loop;
        conn->fd      = client_fd;
        conn->handler = server->handler;
        conn->opts    = server->opts;

        xylem_queue_init(&conn->write_queue);

        conn->io = xylem_loop_create_io(loop, client_fd);
        if (!conn->io) {
            platform_socket_close(client_fd);
            free(conn);
            continue;
        }

        if (_tcp_setup_conn(conn) != 0) {
            xylem_loop_destroy_io(conn->io);
            platform_socket_close(client_fd);
            free(conn);
            continue;
        }

        conn->server = server;
        xylem_list_insert_tail(&server->connections,
                               &conn->server_node);

        /* Capture peer address from the accepted socket. */
        socklen_t peer_len = sizeof(conn->peer_addr.storage);
        getpeername(client_fd, (struct sockaddr*)&conn->peer_addr.storage,
                    &peer_len);

        xylem_logi("tcp server fd=%d accepted conn fd=%d",
                   (int)server->fd, (int)client_fd);

        if (server->handler && server->handler->on_accept) {
            server->handler->on_accept(server, conn);
        }

        if (server->closing) {
            break;
        }
    }
}

/* Post callback: free a server after the current iteration. */
static void _tcp_server_free_cb(xylem_loop_t* loop,
                                xylem_loop_post_t* req,
                                void* ud) {
    (void)loop;
    (void)req;
    free(ud);
}

void xylem_tcp_close_server(xylem_tcp_server_t* server) {
    if (server->closing) {
        return;
    }

    xylem_logi("tcp server fd=%d closing", (int)server->fd);
    server->closing = true;

    xylem_loop_destroy_io(server->io);
    server->io = NULL;
    platform_socket_close(server->fd);

    while (!xylem_list_empty(&server->connections)) {
        xylem_list_node_t* node = xylem_list_head(&server->connections);
        xylem_tcp_conn_t* conn =
            xylem_list_entry(node, xylem_tcp_conn_t, server_node);
        xylem_list_remove(&server->connections, node);
        conn->server = NULL;
        xylem_tcp_close(conn);
    }

    xylem_loop_post(server->loop, _tcp_server_free_cb, server);
}

void xylem_tcp_close(xylem_tcp_conn_t* conn) {
    if (conn->state == TCP_STATE_CLOSING ||
        conn->state == TCP_STATE_CLOSED) {
        return;
    }

    xylem_logi("tcp conn fd=%d graceful close requested",
               (int)conn->fd);
    conn->state = TCP_STATE_CLOSING;

    if (xylem_queue_empty(&conn->write_queue)) {
        shutdown(conn->fd, PLATFORM_SHUT_WR);
        _tcp_destroy_conn(conn, 0);
    }
}

int xylem_tcp_send(xylem_tcp_conn_t* conn, const void* data, size_t len) {
    if (conn->state == TCP_STATE_CLOSING ||
        conn->state == TCP_STATE_CLOSED) {
        xylem_logd("tcp conn fd=%d send rejected (state=%d)",
                   (int)conn->fd, conn->state);
        return -1;
    }

    _tcp_write_req_t* req = malloc(sizeof(*req) + len);
    if (!req) {
        return -1;
    }

    req->data   = (char*)req + sizeof(*req);
    req->len    = len;
    req->offset = 0;
    memcpy(req->data, data, len);

    bool was_empty = xylem_queue_empty(&conn->write_queue);
    xylem_queue_enqueue(&conn->write_queue, &req->node);
    xylem_logd("tcp conn fd=%d enqueue write %zu bytes", (int)conn->fd, len);

    if (was_empty) {
        xylem_loop_start_io(conn->io,
                            PLATFORM_POLLER_RD_OP | PLATFORM_POLLER_WR_OP,
                            _tcp_conn_io_cb, conn);

        if (conn->opts.write_timeout_ms > 0 && conn->write_timer) {
            xylem_loop_start_timer(conn->write_timer,
                                   _tcp_write_timeout_cb,
                                   conn, conn->opts.write_timeout_ms, 0);
        }
    }

    return 0;
}

const xylem_addr_t* xylem_tcp_get_peer_addr(xylem_tcp_conn_t* conn) {
    return &conn->peer_addr;
}

xylem_loop_t* xylem_tcp_get_loop(xylem_tcp_conn_t* conn) {
    return conn->loop;
}

void* xylem_tcp_get_userdata(xylem_tcp_conn_t* conn) {
    return conn->userdata;
}

void xylem_tcp_set_userdata(xylem_tcp_conn_t* conn, void* ud) {
    conn->userdata = ud;
}

xylem_tcp_conn_t* xylem_tcp_dial(xylem_loop_t* loop,
                                 xylem_addr_t* addr,
                                 xylem_tcp_handler_t* handler,
                                 xylem_tcp_opts_t* opts) {
    xylem_tcp_conn_t* conn = calloc(1, sizeof(*conn));
    if (!conn) {
        return NULL;
    }

    _tcp_dial_priv_t* dial = calloc(1, sizeof(*dial));
    if (!dial) {
        free(conn);
        return NULL;
    }

    dial->conn            = conn;
    dial->peer_addr       = *addr;
    dial->reconnect_count = 0;
    dial->reconnect_cb    = _tcp_reconnect_timeout_cb;
    conn->dial            = dial;
    conn->peer_addr       = *addr;

    if (opts) {
        conn->opts = *opts;
    }

    if (conn->opts.read_buf_size == 0) {
        conn->opts.read_buf_size = TCP_DEFAULT_READ_BUF_SIZE;
    }

    conn->loop    = loop;
    conn->handler = handler;
    conn->state   = TCP_STATE_CONNECTING;

    xylem_queue_init(&conn->write_queue);

    _tcp_resolve_hostport(addr, dial->host, sizeof(dial->host),
                          dial->port_str, sizeof(dial->port_str));

    bool connected = false;
    platform_sock_t fd = platform_socket_dial(dial->host, dial->port_str,
                                              SOCK_STREAM,
                                              &connected, true);

    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("tcp dial: socket creation failed for %s:%s",
                   dial->host, dial->port_str);
        free(dial);
        free(conn);
        return NULL;
    }

    conn->fd = fd;
    xylem_logi("tcp dial fd=%d to %s:%s", (int)fd,
               dial->host, dial->port_str);

    conn->io = xylem_loop_create_io(loop, conn->fd);
    if (!conn->io) {
        platform_socket_close(fd);
        free(dial);
        free(conn);
        return NULL;
    }

    if (conn->opts.connect_timeout_ms > 0) {
        dial->connect_timer = xylem_loop_create_timer(loop);
    }
    if (conn->opts.reconnect_max > 0) {
        dial->reconnect_timer = xylem_loop_create_timer(loop);
    }

    if (connected) {
        _tcp_conn_connected_cb(conn);
    } else {
        xylem_loop_start_io(conn->io, PLATFORM_POLLER_WR_OP,
                            _tcp_try_connect, conn);

        if (conn->opts.connect_timeout_ms > 0 && dial->connect_timer) {
            xylem_loop_start_timer(dial->connect_timer,
                                   _tcp_connect_timeout_cb,
                                   conn, conn->opts.connect_timeout_ms, 0);
        }
    }

    return conn;
}

xylem_tcp_server_t* xylem_tcp_listen(xylem_loop_t* loop,
                                     xylem_addr_t* addr,
                                     xylem_tcp_handler_t* handler,
                                     xylem_tcp_opts_t* opts) {
    xylem_tcp_server_t* server = calloc(1, sizeof(*server));
    if (!server) {
        return NULL;
    }

    if (opts) {
        server->opts = *opts;
    }

    if (server->opts.read_buf_size == 0) {
        server->opts.read_buf_size = TCP_DEFAULT_READ_BUF_SIZE;
    }

    server->loop    = loop;
    server->handler = handler;
    server->closing = false;

    xylem_list_init(&server->connections);

    char host[INET6_ADDRSTRLEN];
    char port_str[8];
    _tcp_resolve_hostport(addr, host, sizeof(host),
                          port_str, sizeof(port_str));

    platform_sock_t fd = platform_socket_listen(host, port_str,
                                                SOCK_STREAM, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        free(server);
        xylem_loge("tcp listen: socket creation failed for %s:%s",
                   host, port_str);
        return NULL;
    }

    server->fd = fd;

    server->io = xylem_loop_create_io(loop, server->fd);
    if (!server->io) {
        platform_socket_close(fd);
        free(server);
        return NULL;
    }
    xylem_loop_start_io(server->io, PLATFORM_POLLER_RD_OP,
                        _tcp_server_io_cb, server);

    xylem_logi("tcp server fd=%d listening on %s:%s",
               (int)fd, host, port_str);
    return server;
}

void* xylem_tcp_server_get_userdata(xylem_tcp_server_t* server) {
    return server->userdata;
}

void xylem_tcp_server_set_userdata(xylem_tcp_server_t* server, void* ud) {
    server->userdata = ud;
}
