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

#include <inttypes.h>
#include <string.h>

#define TCP_DEFAULT_READ_BUF_SIZE 65536

typedef enum {
    _TCP_STATE_CONNECTING,
    _TCP_STATE_CONNECTED,
    _TCP_STATE_CLOSING,
    _TCP_STATE_CLOSED,
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
    xylem_loop_timer_t    connect_timer;
    xylem_loop_timer_t    reconnect_timer;
    xylem_addr_t          peer_addr;
    uint32_t              reconnect_count;
    xylem_tcp_conn_t*     conn;
    xylem_loop_timer_fn_t reconnect_cb;
    char                  host[INET6_ADDRSTRLEN];
    char                  port_str[8];
} _tcp_dial_priv_t;

struct xylem_tcp_conn_s {
    xylem_loop_t*         loop;
    xylem_loop_io_t       io;
    platform_sock_t       fd;
    xylem_tcp_handler_t*  handler;
    xylem_tcp_opts_t      opts;
    _tcp_state_t     state;
    uint8_t*              read_buf;
    size_t                read_len;
    size_t                read_cap;
    xylem_queue_t         write_queue;
    xylem_loop_timer_t    read_timer;
    xylem_loop_timer_t    write_timer;
    xylem_loop_timer_t    heartbeat_timer;
    _tcp_dial_priv_t*     dial;
    xylem_list_node_t     server_node;
    xylem_tcp_server_t*   server;
    void*                 userdata;
    xylem_loop_post_t     free_post;
};

struct xylem_tcp_server_s {
    xylem_loop_t*        loop;
    xylem_loop_io_t      io;
    platform_sock_t      fd;
    xylem_tcp_handler_t* handler;
    xylem_tcp_opts_t     opts;
    xylem_list_t         connections;
    void*                userdata;
    bool                 closing;
    xylem_loop_post_t    free_post;
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

        /* Need at least the fixed header to read the length field. */
        if (avail < hdr_sz) {
            return 0;
        }

        uint32_t effective_hdr = hdr_sz;
        uint64_t payload_len = 0;

        if (conn->opts.framing.length.coding == XYLEM_TCP_LENGTH_FIXEDINT) {
            if (len_sz == 0 || len_sz > 8) {
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
            /* VARINT */
            size_t pos = (size_t)len_off;
            if (!xylem_varint_decode(data, avail, &pos, &payload_len)) {
                if (avail < hdr_sz + 10) {
                    return 0;
                }
                return -1;
            }
            /* Varint may change effective header size. */
            uint32_t varint_bytes = (uint32_t)(pos - len_off);
            effective_hdr = hdr_sz + varint_bytes - len_sz;
        }

        int64_t frame_size = (int64_t)effective_hdr + (int64_t)payload_len +
                             (int64_t)adj;
        if (frame_size <= 0) {
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
            return -1;
        }

        int rc = conn->opts.framing.custom.parse(data, avail);

        if (rc > 0) {
            if ((size_t)rc > avail) {
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
                                 xylem_loop_timer_t* timer) {
    (void)loop;
    xylem_tcp_conn_t* conn =
        xylem_list_entry(timer, xylem_tcp_conn_t, read_timer);

    if (conn->handler && conn->handler->on_timeout) {
        conn->handler->on_timeout(conn, XYLEM_TCP_TIMEOUT_READ);
    }
}

static void _tcp_write_timeout_cb(xylem_loop_t* loop,
                                  xylem_loop_timer_t* timer) {
    (void)loop;
    xylem_tcp_conn_t* conn =
        xylem_list_entry(timer, xylem_tcp_conn_t, write_timer);

    if (conn->handler && conn->handler->on_timeout) {
        conn->handler->on_timeout(conn, XYLEM_TCP_TIMEOUT_WRITE);
    }
}

static void _tcp_connect_timeout_cb(xylem_loop_t* loop,
                                    xylem_loop_timer_t* timer) {
    (void)loop;
    _tcp_dial_priv_t* dial =
        xylem_list_entry(timer, _tcp_dial_priv_t, connect_timer);
    xylem_tcp_conn_t* conn = dial->conn;

    if (conn->handler && conn->handler->on_timeout) {
        conn->handler->on_timeout(conn, XYLEM_TCP_TIMEOUT_CONNECT);
    }
}

static void _tcp_heartbeat_timeout_cb(xylem_loop_t* loop,
                               xylem_loop_timer_t* timer) {
    (void)loop;
    xylem_tcp_conn_t* conn =
        xylem_list_entry(timer, xylem_tcp_conn_t, heartbeat_timer);

    if (conn->handler && conn->handler->on_heartbeat_miss) {
        conn->handler->on_heartbeat_miss(conn);
    }
}

/* Post callback: free a connection after the current iteration. */
static void _tcp_conn_free_cb(xylem_loop_t* loop,
                               xylem_loop_post_t* req) {
    (void)loop;
    xylem_tcp_conn_t* conn =
        xylem_list_entry(req, xylem_tcp_conn_t, free_post);
    free(conn);
}

static void _tcp_destroy_conn(xylem_tcp_conn_t* conn, int err) {
    conn->state = _TCP_STATE_CLOSED;
    xylem_logd("tcp conn fd=%d destroy err=%d",
               (int)conn->fd, err);

    if (conn->server) {
        xylem_list_remove(&conn->server->connections, &conn->server_node);
        conn->server = NULL;
    }

    if (conn->dial) {
        if (conn->dial->connect_timer.loop) {
            xylem_loop_stop_timer(&conn->dial->connect_timer);
            xylem_loop_deinit_timer(&conn->dial->connect_timer);
        }
        if (conn->dial->reconnect_timer.loop) {
            xylem_loop_stop_timer(&conn->dial->reconnect_timer);
            xylem_loop_deinit_timer(&conn->dial->reconnect_timer);
        }
    }

    if (conn->read_timer.loop) {
        xylem_loop_stop_timer(&conn->read_timer);
        xylem_loop_deinit_timer(&conn->read_timer);
    }
    if (conn->write_timer.loop) {
        xylem_loop_stop_timer(&conn->write_timer);
        xylem_loop_deinit_timer(&conn->write_timer);
    }
    if (conn->heartbeat_timer.loop) {
        xylem_loop_stop_timer(&conn->heartbeat_timer);
        xylem_loop_deinit_timer(&conn->heartbeat_timer);
    }

    xylem_loop_stop_io(&conn->io);
    xylem_loop_deinit_io(&conn->io);
    platform_socket_close(conn->fd);

    if (conn->read_buf) {
        free(conn->read_buf);
        conn->read_buf = NULL;
    }

    if (conn->dial) {
        free(conn->dial);
        conn->dial = NULL;
    }

    if (conn->handler && conn->handler->on_close) {
        conn->handler->on_close(conn, err);
    }

    conn->free_post.cb = _tcp_conn_free_cb;
    xylem_loop_post(conn->loop, &conn->free_post);
}

static void _tcp_close_conn(xylem_tcp_conn_t* conn, int err) {
    if (conn->state == _TCP_STATE_CLOSED ||
        conn->state == _TCP_STATE_CLOSING) {
        return;
    }

    xylem_logd("tcp conn fd=%d start_close err=%d",
               (int)conn->fd, err);
    conn->state = _TCP_STATE_CLOSING;

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
            xylem_logw("tcp conn fd=%d recv error=%d",
                       (int)conn->fd, err);
            _tcp_close_conn(conn, err);
            return;
        }

        conn->read_len += (size_t)nread;
        xylem_logd("tcp conn fd=%d recv %zd bytes",
                   (int)conn->fd, nread);

        /* Drain complete frames, then compact the buffer. */
        size_t consumed = 0;
        for (;;) {
            void*  frame_data = NULL;
            size_t frame_len  = 0;
            ssize_t rc = _tcp_extract_frame(conn, &frame_data, &frame_len);

            if (rc > 0) {
                if (conn->handler && conn->handler->on_read) {
                    conn->handler->on_read(conn, frame_data, frame_len);
                }

                consumed += (size_t)rc;
                conn->read_buf += (size_t)rc;
                conn->read_len -= (size_t)rc;

                if (conn->state == _TCP_STATE_CLOSED ||
                    conn->state == _TCP_STATE_CLOSING) {
                    return;
                }
            } else if (rc == 0) {
                break;
            } else {
                _tcp_close_conn(conn, -1);
                return;
            }
        }

        /* Restore read_buf pointer and compact remaining bytes. */
        if (consumed > 0) {
            conn->read_buf -= consumed;
            if (conn->read_len > 0) {
                memmove(conn->read_buf,
                        conn->read_buf + consumed,
                        conn->read_len);
            }
        }

        if ((size_t)nread < space) {
            break;
        }
    }

    if (conn->opts.heartbeat_ms > 0) {
        xylem_loop_reset_timer(&conn->heartbeat_timer,
                               conn->opts.heartbeat_ms);
    }

    if (conn->opts.read_timeout_ms > 0) {
        xylem_loop_reset_timer(&conn->read_timer,
                               conn->opts.read_timeout_ms);
    }
}

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

            xylem_logw("tcp conn fd=%d send error=%d",
                       (int)conn->fd, err);

            /**
             * If already in graceful close, skip start_close (which
             * would bail on CLOSING state) and destroy directly.
             */
            if (conn->state == _TCP_STATE_CLOSING) {
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

            /* User may have called close from on_write_done. */
            if (conn->state == _TCP_STATE_CLOSED) {
                return;
            }

            if (conn->opts.write_timeout_ms > 0 &&
                !xylem_queue_empty(&conn->write_queue)) {
                xylem_loop_reset_timer(&conn->write_timer,
                                       conn->opts.write_timeout_ms);
            }
        } else {
            xylem_logd("tcp conn fd=%d partial write %zd/%zu",
                       (int)conn->fd, n, rem);
            return;
        }
    }

    if (conn->opts.write_timeout_ms > 0) {
        xylem_loop_stop_timer(&conn->write_timer);
    }

    if (conn->state == _TCP_STATE_CLOSING) {
        xylem_logd("tcp conn fd=%d write queue drained, shutting down",
                   (int)conn->fd);
        shutdown(conn->fd, PLATFORM_SHUT_WR);
        _tcp_destroy_conn(conn, 0);
    }
}

static void _tcp_conn_io_cb(xylem_loop_t* loop,
                            xylem_loop_io_t* io,
                            platform_poller_op_t revents) {
    (void)loop;
    xylem_tcp_conn_t* conn =
        xylem_list_entry(io, xylem_tcp_conn_t, io);

    if (revents & PLATFORM_POLLER_RD_OP) {
        _tcp_conn_readable_cb(conn);
    }

    if (conn->state == _TCP_STATE_CLOSED) {
        return;
    }

    if (revents & PLATFORM_POLLER_WR_OP) {
        _tcp_flush_writes(conn);

        /* Re-arm to read-only once the write queue drains */
        if (conn->state == _TCP_STATE_CONNECTED &&
            xylem_queue_empty(&conn->write_queue)) {
            xylem_loop_start_io(&conn->io, PLATFORM_POLLER_RD_OP,
                                _tcp_conn_io_cb);
        }
    }
}

/**
 * Common setup for a newly connected socket: allocate read buffer,
 * start IO, start heartbeat/read timers. Does NOT call any handler
 * callback.
 */
static int _tcp_setup_conn(xylem_tcp_conn_t* conn) {
    conn->state    = _TCP_STATE_CONNECTED;
    conn->read_buf = malloc(conn->opts.read_buf_size);
    if (!conn->read_buf) {
        return -1;
    }
    conn->read_len = 0;
    conn->read_cap = conn->opts.read_buf_size;

    if (xylem_loop_start_io(&conn->io, PLATFORM_POLLER_RD_OP,
                            _tcp_conn_io_cb) != 0) {
        free(conn->read_buf);
        conn->read_buf = NULL;
        return -1;
    }

    if (conn->opts.heartbeat_ms > 0) {
        if (!conn->heartbeat_timer.loop) {
            xylem_loop_init_timer(conn->loop, &conn->heartbeat_timer);
        }
        xylem_loop_start_timer(&conn->heartbeat_timer,
                               _tcp_heartbeat_timeout_cb,
                               conn->opts.heartbeat_ms,
                               conn->opts.heartbeat_ms);
    }

    if (conn->opts.read_timeout_ms > 0) {
        if (!conn->read_timer.loop) {
            xylem_loop_init_timer(conn->loop, &conn->read_timer);
        }
        xylem_loop_start_timer(&conn->read_timer,
                               _tcp_read_timeout_cb,
                               conn->opts.read_timeout_ms, 0);
    }

    if (conn->opts.write_timeout_ms > 0) {
        if (!conn->write_timer.loop) {
            xylem_loop_init_timer(conn->loop, &conn->write_timer);
        }
    }

    return 0;
}

static void _tcp_conn_connected_cb(xylem_tcp_conn_t* conn) {
    if (_tcp_setup_conn(conn) != 0) {
        _tcp_close_conn(conn, -1);
        return;
    }
    xylem_logi("tcp conn fd=%d connected", (int)conn->fd);

    if (conn->handler && conn->handler->on_connect) {
        conn->handler->on_connect(conn);
    }
}

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

    xylem_loop_start_timer(&dial->reconnect_timer, cb, delay, 0);
    xylem_logi("tcp conn fd=%d scheduling reconnect #%u in %" PRIu64 " ms",
               (int)conn->fd, dial->reconnect_count + 1,
               delay);
}

static void _tcp_try_connect(xylem_loop_t* loop,
                             xylem_loop_io_t* io,
                             platform_poller_op_t revents) {
    (void)loop;
    (void)revents;
    xylem_tcp_conn_t* conn =
        xylem_list_entry(io, xylem_tcp_conn_t, io);
    _tcp_dial_priv_t* dial = conn->dial;

    int err    = 0;
    socklen_t errlen = sizeof(err);

    getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen);

    xylem_logd("tcp conn fd=%d connect result SO_ERROR=%d",
               (int)conn->fd, err);

    if (conn->opts.connect_timeout_ms > 0) {
        xylem_loop_stop_timer(&dial->connect_timer);
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
                                      xylem_loop_timer_t* timer) {
    (void)loop;
    _tcp_dial_priv_t* dial =
        xylem_list_entry(timer, _tcp_dial_priv_t, reconnect_timer);
    xylem_tcp_conn_t* conn = dial->conn;

    xylem_loop_stop_io(&conn->io);
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
    /**
     * Re-use existing IO handle -- just update the fd and reset
     * registration state.  Do NOT call io_init again because that
     * would increment active_count a second time.
     */
    conn->io.sqe.fd     = fd;
    conn->io.registered  = false;
    conn->state = _TCP_STATE_CONNECTING;
    dial->reconnect_count++;

    if (connected) {
        _tcp_conn_connected_cb(conn);
    } else {
        xylem_loop_start_io(&conn->io, PLATFORM_POLLER_WR_OP,
                            _tcp_try_connect);

        if (conn->opts.connect_timeout_ms > 0) {
            xylem_loop_start_timer(&dial->connect_timer,
                                   _tcp_connect_timeout_cb,
                                   conn->opts.connect_timeout_ms, 0);
        }
    }
}

static void _tcp_server_io_cb(xylem_loop_t* loop,
                              xylem_loop_io_t* io,
                              platform_poller_op_t revents) {
    (void)revents;
    xylem_tcp_server_t* server =
        xylem_list_entry(io, xylem_tcp_server_t, io);

    for (;;) {
        platform_sock_t client_fd =
            platform_socket_accept(server->fd, true);

        if (client_fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                break;
            }
            xylem_logw("tcp server fd=%d accept error=%d",
                       (int)server->fd, err);
            continue;
        }

        xylem_tcp_conn_t* conn = calloc(1, sizeof(*conn));
        if (!conn) {
            platform_socket_close(client_fd);
            continue;
        }

        conn->loop    = loop;
        conn->fd      = client_fd;
        conn->handler = server->handler;
        conn->opts    = server->opts;

        xylem_queue_init(&conn->write_queue);

        xylem_loop_init_io(loop, &conn->io, client_fd);

        if (_tcp_setup_conn(conn) != 0) {
            xylem_loop_deinit_io(&conn->io);
            platform_socket_close(client_fd);
            free(conn);
            continue;
        }

        conn->server = server;
        conn->userdata = server->userdata;
        xylem_list_insert_tail(&server->connections,
                               &conn->server_node);

        xylem_logi("tcp server fd=%d accepted conn fd=%d",
                   (int)server->fd, (int)client_fd);

        if (server->handler && server->handler->on_accept) {
            server->handler->on_accept(conn);
        }

        if (server->closing) {
            break;
        }
    }
}

/* Post callback: free a server after the current iteration. */
static void _tcp_server_free_cb(xylem_loop_t* loop,
                                xylem_loop_post_t* req) {
    (void)loop;
    xylem_tcp_server_t* server =
        xylem_list_entry(req, xylem_tcp_server_t, free_post);
    free(server);
}

void xylem_tcp_close_server(xylem_tcp_server_t* server) {
    if (server->closing) {
        return;
    }

    xylem_logi("tcp server fd=%d closing", (int)server->fd);
    server->closing = true;

    xylem_loop_stop_io(&server->io);
    xylem_loop_deinit_io(&server->io);
    platform_socket_close(server->fd);

    /**
     * Detach all accepted connections from the server list so their
     * server_node pointers don't dangle after server is freed.
     */
    while (!xylem_list_empty(&server->connections)) {
        xylem_list_node_t* node = xylem_list_head(&server->connections);
        xylem_list_remove(&server->connections, node);
        xylem_tcp_conn_t* conn =
            xylem_list_entry(node, xylem_tcp_conn_t, server_node);
        conn->server = NULL;
    }

    server->free_post.cb = _tcp_server_free_cb;
    xylem_loop_post(server->loop, &server->free_post);
}

void xylem_tcp_close(xylem_tcp_conn_t* conn) {
    if (conn->state == _TCP_STATE_CLOSING ||
        conn->state == _TCP_STATE_CLOSED) {
        return;
    }

    xylem_logi("tcp conn fd=%d graceful close requested",
               (int)conn->fd);
    conn->state = _TCP_STATE_CLOSING;

    if (xylem_queue_empty(&conn->write_queue)) {
        shutdown(conn->fd, PLATFORM_SHUT_WR);
        _tcp_destroy_conn(conn, 0);
    }
    /**
     * Otherwise, _tcp_flush_writes will complete the close when
     * the write queue empties and state is CLOSING.
     */
}

int xylem_tcp_send(xylem_tcp_conn_t* conn, const void* data, size_t len) {
    if (conn->state == _TCP_STATE_CLOSING ||
        conn->state == _TCP_STATE_CLOSED) {
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
        xylem_loop_start_io(&conn->io,
                            PLATFORM_POLLER_RD_OP | PLATFORM_POLLER_WR_OP,
                            _tcp_conn_io_cb);

        if (conn->opts.write_timeout_ms > 0) {
            xylem_loop_start_timer(&conn->write_timer,
                                   _tcp_write_timeout_cb,
                                   conn->opts.write_timeout_ms, 0);
        }
    }

    return 0;
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

    if (opts) {
        conn->opts = *opts;
    }

    if (conn->opts.read_buf_size == 0) {
        conn->opts.read_buf_size = TCP_DEFAULT_READ_BUF_SIZE;
    }

    conn->loop    = loop;
    conn->handler = handler;
    conn->state   = _TCP_STATE_CONNECTING;

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

    xylem_loop_init_io(loop, &conn->io, conn->fd);

    if (conn->opts.connect_timeout_ms > 0) {
        xylem_loop_init_timer(loop, &dial->connect_timer);
    }
    if (conn->opts.reconnect_max > 0) {
        xylem_loop_init_timer(loop, &dial->reconnect_timer);
    }

    if (connected) {
        _tcp_conn_connected_cb(conn);
    } else {
        xylem_loop_start_io(&conn->io, PLATFORM_POLLER_WR_OP,
                            _tcp_try_connect);

        if (conn->opts.connect_timeout_ms > 0) {
            xylem_loop_start_timer(&dial->connect_timer,
                                   _tcp_connect_timeout_cb,
                                   conn->opts.connect_timeout_ms, 0);
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

    xylem_loop_init_io(loop, &server->io, server->fd);
    xylem_loop_start_io(&server->io, PLATFORM_POLLER_RD_OP,
                        _tcp_server_io_cb);

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
