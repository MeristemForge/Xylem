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
#include "xylem/xylem-ringbuf.h"
#include "xylem/xylem-queue.h"
#include "xylem/xylem-list.h"

#include <string.h>

#define XYLEM_TCP_DEFAULT_READ_BUF_SIZE 65536

typedef struct xylem_tcp_write_req_s {
    xylem_queue_node_t node;
    void*              data;
    size_t             len;
    size_t             offset;
} xylem_tcp_write_req_t;

/**
 * Dial-private state: only allocated for outbound (dialed) connections.
 * Holds connect/reconnect timers, peer address, and retry counter.
 */
typedef struct _tcp_dial_priv_s {
    xylem_loop_timer_t  connect_timer;
    xylem_loop_timer_t  reconnect_timer;
    xylem_addr_t        peer_addr;
    uint32_t            reconnect_count;
    xylem_tcp_conn_t*   conn;
} _tcp_dial_priv_t;

struct xylem_tcp_conn_s {
    xylem_loop_t*         loop;
    xylem_loop_io_t       io;
    platform_sock_t       fd;
    xylem_tcp_handler_t*  handler;
    xylem_tcp_opts_t      opts;
    xylem_tcp_state_t     state;
    xylem_ringbuf_t*      read_buf;
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
    bool                 closing;
    xylem_loop_post_t    free_post;
};

/**
 * Extract one complete frame from the connection's ringbuf.
 * Returns > 0 on success (bytes consumed from ringbuf),
 * 0 if data insufficient, < 0 on error.
 * On success, *frame_out points to malloc'd frame data and
 * *frame_len_out is set. The caller must free *frame_out.
 */
static ssize_t _tcp_extract_frame(xylem_tcp_conn_t* conn,
                                  void** frame_out,
                                  size_t* frame_len_out) {
    xylem_ringbuf_t* ring  = conn->read_buf;
    size_t           avail = xylem_ringbuf_len(ring);

    if (avail == 0) return 0;

    switch (conn->opts.framing.type) {

    case XYLEM_TCP_FRAME_NONE: {
        /* Return all available bytes */
        void* buf = malloc(avail);
        if (!buf) return -1;

        xylem_ringbuf_read(ring, buf, avail);
        *frame_out     = buf;
        *frame_len_out = avail;
        return (ssize_t)avail;
    }

    case XYLEM_TCP_FRAME_FIXED: {
        size_t fsz = conn->opts.framing.fixed.frame_size;
        if (fsz == 0) return -1;
        if (avail < fsz) return 0;

        void* buf = malloc(fsz);
        if (!buf) return -1;

        xylem_ringbuf_read(ring, buf, fsz);
        *frame_out     = buf;
        *frame_len_out = fsz;
        return (ssize_t)fsz;
    }

    case XYLEM_TCP_FRAME_LENGTH: {
        uint8_t hdr_sz = conn->opts.framing.length.header_bytes;
        if (hdr_sz != 1 && hdr_sz != 2 && hdr_sz != 4) return -1;
        if (avail < hdr_sz) return 0;

        /* Peek header bytes without consuming */
        uint8_t hdr[4];
        xylem_ringbuf_peek(ring, hdr, hdr_sz);

        /* Decode payload length from header bytes */
        size_t payload_len = 0;
        if (hdr_sz == 1) {
            payload_len = hdr[0];
        } else if (hdr_sz == 2) {
            if (conn->opts.framing.length.big_endian) {
                payload_len = ((size_t)hdr[0] << 8) | hdr[1];
            } else {
                payload_len = hdr[0] | ((size_t)hdr[1] << 8);
            }
        } else { /* hdr_sz == 4 */
            if (conn->opts.framing.length.big_endian) {
                payload_len = ((size_t)hdr[0] << 24) |
                              ((size_t)hdr[1] << 16) |
                              ((size_t)hdr[2] << 8)  |
                              hdr[3];
            } else {
                payload_len = hdr[0]                  |
                              ((size_t)hdr[1] << 8)   |
                              ((size_t)hdr[2] << 16)  |
                              ((size_t)hdr[3] << 24);
            }
        }

        size_t total = (size_t)hdr_sz + payload_len;
        if (avail < total) return 0;

        void* buf = malloc(payload_len);
        if (!buf) return -1;

        /**
         * Discard header, then read payload — two ringbuf ops but
         * avoids memmove on the payload data.
         */
        xylem_ringbuf_read(ring, hdr, hdr_sz);
        xylem_ringbuf_read(ring, buf, payload_len);

        *frame_out     = buf;
        *frame_len_out = payload_len;
        return (ssize_t)total;
    }

    case XYLEM_TCP_FRAME_DELIM: {
        const char* delim     = conn->opts.framing.delim.delim;
        size_t      delim_len = conn->opts.framing.delim.delim_len;
        if (!delim || delim_len == 0) return -1;

        /* Peek all available data into a temporary buffer */
        void* peek_buf = malloc(avail);
        if (!peek_buf) return -1;

        xylem_ringbuf_peek(ring, peek_buf, avail);

        /* Search for delimiter */
        const char* data = (const char*)peek_buf;
        ssize_t     found_at = -1;

        if (delim_len == 1) {
            /* Fast path: single-byte delimiter */
            const char* p = (const char*)memchr(data, delim[0], avail);
            if (p) found_at = (ssize_t)(p - data);
        } else {
            for (size_t i = 0; i + delim_len <= avail; i++) {
                if (memcmp(data + i, delim, delim_len) == 0) {
                    found_at = (ssize_t)i;
                    break;
                }
            }
        }

        if (found_at < 0) {
            free(peek_buf);
            return 0; /* delimiter not found yet */
        }

        size_t frame_len   = (size_t)found_at;
        size_t consume_len = frame_len + delim_len;

        /* Consume frame + delimiter from ringbuf */
        xylem_ringbuf_read(ring, peek_buf, consume_len);

        /**
         * Reuse peek_buf as frame output (realloc to exact size).
         * For zero-length frames, free and return NULL.
         */
        if (frame_len == 0) {
            free(peek_buf);
            *frame_out     = NULL;
            *frame_len_out = 0;
        } else {
            void* trimmed = realloc(peek_buf, frame_len);
            *frame_out     = trimmed ? trimmed : peek_buf;
            *frame_len_out = frame_len;
        }
        return (ssize_t)consume_len;
    }

    case XYLEM_TCP_FRAME_CUSTOM: {
        if (!conn->opts.framing.custom.parse) return -1;

        /* Peek all available data */
        void* peek_buf = malloc(avail);
        if (!peek_buf) return -1;

        xylem_ringbuf_peek(ring, peek_buf, avail);

        int rc = conn->opts.framing.custom.parse(peek_buf, avail);

        if (rc > 0) {
            if ((size_t)rc > avail) {
                free(peek_buf);
                return -1; /* parser returned invalid length */
            }

            /* Consume rc bytes from ringbuf */
            xylem_ringbuf_read(ring, peek_buf, (size_t)rc);

            /* Reuse peek_buf as frame output */
            if ((size_t)rc < avail) {
                void* trimmed = realloc(peek_buf, (size_t)rc);
                *frame_out = trimmed ? trimmed : peek_buf;
            } else {
                *frame_out = peek_buf;
            }
            *frame_len_out = (size_t)rc;
            return rc;
        }

        free(peek_buf);
        return rc; /* 0 = insufficient data, < 0 = error */
    }

    default:
        return -1;
    }
}

static void _tcp_conn_io_cb(xylem_loop_t* loop,
                            xylem_loop_io_t* io,
                            platform_poller_op_t revents);
static void _tcp_flush_writes(xylem_tcp_conn_t* conn);
static void _tcp_try_connect(xylem_loop_t* loop,
                             xylem_loop_io_t* io,
                             platform_poller_op_t revents);
static void _tcp_reconnect_timer_cb(xylem_loop_t* loop,
                                    xylem_loop_timer_t* timer);

/* Extract host string and port from xylem_addr_t into caller buffers. */
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

static void _tcp_heartbeat_cb(xylem_loop_t* loop,
                               xylem_loop_timer_t* timer) {
    (void)loop;
    xylem_tcp_conn_t* conn =
        xylem_list_entry(timer, xylem_tcp_conn_t, heartbeat_timer);

    if (conn->handler && conn->handler->on_heartbeat_miss) {
        conn->handler->on_heartbeat_miss(conn);
    }
}

/**
 * Common setup for a newly connected socket: init ringbuf, start IO,
 * start heartbeat/read timers. Does NOT call any handler callback.
 */
static void _tcp_setup_conn(xylem_tcp_conn_t* conn) {
    conn->state    = XYLEM_TCP_STATE_CONNECTED;
    conn->read_buf = xylem_ringbuf_create(1, conn->opts.read_buf_size);

    xylem_loop_start_io(&conn->io, PLATFORM_POLLER_RD_OP,
                        _tcp_conn_io_cb);

    if (conn->opts.heartbeat_ms > 0) {
        xylem_loop_start_timer(&conn->heartbeat_timer,
                               _tcp_heartbeat_cb,
                               conn->opts.heartbeat_ms,
                               conn->opts.heartbeat_ms);
    }

    if (conn->opts.read_timeout_ms > 0) {
        xylem_loop_start_timer(&conn->read_timer,
                               _tcp_read_timeout_cb,
                               conn->opts.read_timeout_ms, 0);
    }
}

static void _tcp_conn_connected_cb(xylem_tcp_conn_t* conn) {
    _tcp_setup_conn(conn);
    xylem_logi("tcp conn fd=%d connected", (int)conn->fd);

    if (conn->handler && conn->handler->on_connect) {
        conn->handler->on_connect(conn);
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
    conn->state = XYLEM_TCP_STATE_CLOSED;
    xylem_logd("tcp conn fd=%d destroy err=%d",
               (int)conn->fd, err);

    /* Remove from server's connection list if this is an accepted conn */
    if (conn->server) {
        xylem_list_remove(&conn->server->connections, &conn->server_node);
        conn->server = NULL;
    }

    /* Close dial-only timers if present */
    if (conn->dial) {
        xylem_loop_stop_timer(&conn->dial->connect_timer);
        conn->loop->active_count--;
        xylem_loop_stop_timer(&conn->dial->reconnect_timer);
        conn->loop->active_count--;
    }

    /* Close shared timers */
    xylem_loop_stop_timer(&conn->read_timer);
    conn->loop->active_count--;
    xylem_loop_stop_timer(&conn->write_timer);
    conn->loop->active_count--;
    xylem_loop_stop_timer(&conn->heartbeat_timer);
    conn->loop->active_count--;

    /* Stop IO polling and close the handle */
    xylem_loop_stop_io(&conn->io);
    conn->loop->active_count--;
    platform_socket_close(conn->fd);

    /* Free read buffer */
    if (conn->read_buf) {
        xylem_ringbuf_destroy(conn->read_buf);
        conn->read_buf = NULL;
    }

    /* Free dial-private state */
    if (conn->dial) {
        free(conn->dial);
        conn->dial = NULL;
    }

    /* Notify user */
    if (conn->handler && conn->handler->on_close) {
        conn->handler->on_close(conn, err);
    }

    /* Defer free to next loop iteration */
    conn->free_post.cb = _tcp_conn_free_cb;
    xylem_loop_post(conn->loop, &conn->free_post);
}

static void _tcp_start_close_conn(xylem_tcp_conn_t* conn, int err) {
    if (conn->state == XYLEM_TCP_STATE_CLOSED ||
        conn->state == XYLEM_TCP_STATE_CLOSING) {
        return;
    }

    xylem_logd("tcp conn fd=%d start_close err=%d",
               (int)conn->fd, err);
    conn->state = XYLEM_TCP_STATE_CLOSING;

    /* Drain write queue, notifying each pending request of the error */
    while (!xylem_queue_empty(&conn->write_queue)) {
        xylem_queue_node_t* node =
            xylem_queue_dequeue(&conn->write_queue);
        xylem_tcp_write_req_t* req =
            xylem_queue_entry(node, xylem_tcp_write_req_t, node);

        if (conn->handler && conn->handler->on_write_done) {
            conn->handler->on_write_done(conn, req->data, req->len, err);
        }

        free(req);
    }

    _tcp_destroy_conn(conn, err);
}

static void _tcp_conn_readable_cb(xylem_tcp_conn_t* conn) {
    char tmp[16384];

    /**
     * Loop recv until EAGAIN to drain the kernel buffer in one
     * IO callback, avoiding repeated poller wakeups under LT.
     */
    for (;;) {
        ssize_t nread = platform_socket_recv(conn->fd, tmp,
                                             (int)sizeof(tmp));

        if (nread == 0) {
            xylem_logd("tcp conn fd=%d peer closed", (int)conn->fd);
            _tcp_start_close_conn(conn, 0);
            return;
        }

        if (nread < 0) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN) {
                break;
            }
            xylem_logw("tcp conn fd=%d recv error=%d",
                       (int)conn->fd, err);
            _tcp_start_close_conn(conn, err);
            return;
        }

        xylem_ringbuf_write(conn->read_buf, tmp, (size_t)nread);
        xylem_logd("tcp conn fd=%d recv %zd bytes",
                   (int)conn->fd, nread);

        /* Short read means kernel buffer is drained */
        if ((size_t)nread < sizeof(tmp)) {
            break;
        }
    }

    /* Reset heartbeat timer on data arrival */
    if (conn->opts.heartbeat_ms > 0) {
        xylem_loop_reset_timer(&conn->heartbeat_timer,
                               conn->opts.heartbeat_ms);
    }

    /* Reset read timeout timer on data arrival */
    if (conn->opts.read_timeout_ms > 0) {
        xylem_loop_reset_timer(&conn->read_timer,
                               conn->opts.read_timeout_ms);
    }

    /* Frame extraction loop */
    for (;;) {
        void*  frame_data = NULL;
        size_t frame_len  = 0;
        ssize_t rc = _tcp_extract_frame(conn, &frame_data, &frame_len);

        if (rc > 0) {
            if (conn->handler && conn->handler->on_read) {
                conn->handler->on_read(conn, frame_data, frame_len);
            }
            free(frame_data);
            xylem_logd("tcp conn fd=%d frame extracted %zd bytes",
                       (int)conn->fd, frame_len);

            /* User may have closed the connection in on_read */
            if (conn->state == XYLEM_TCP_STATE_CLOSED ||
                conn->state == XYLEM_TCP_STATE_CLOSING)
                return;
        } else if (rc == 0) {
            break;
        } else {
            _tcp_start_close_conn(conn, -1);
            return;
        }
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

    /* Guard: on_readable may have destroyed the connection */
    if (conn->state == XYLEM_TCP_STATE_CLOSED) {
        return;
    }

    if (revents & PLATFORM_POLLER_WR_OP) {
        _tcp_flush_writes(conn);
    }
}

static void _tcp_flush_writes(xylem_tcp_conn_t* conn) {
    while (!xylem_queue_empty(&conn->write_queue)) {
        xylem_queue_node_t* front =
            xylem_queue_front(&conn->write_queue);
        xylem_tcp_write_req_t* req =
            xylem_queue_entry(front, xylem_tcp_write_req_t, node);

        char*  ptr = (char*)req->data + req->offset;
        size_t rem = req->len - req->offset;

        ssize_t n = platform_socket_send(conn->fd, ptr, (int)rem);

        if (n < 0) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN) {
                return;
            }

            xylem_logw("tcp conn fd=%d send error=%d",
                       (int)conn->fd, err);

            /**
             * If already in graceful close, skip start_close (which
             * would bail on CLOSING state) and destroy directly.
             */
            if (conn->state == XYLEM_TCP_STATE_CLOSING) {
                /* Drain remaining write queue before destroy */
                while (!xylem_queue_empty(&conn->write_queue)) {
                    xylem_queue_node_t* qn =
                        xylem_queue_dequeue(&conn->write_queue);
                    xylem_tcp_write_req_t* wr =
                        xylem_queue_entry(qn, xylem_tcp_write_req_t, node);
                    if (conn->handler && conn->handler->on_write_done) {
                        conn->handler->on_write_done(conn, wr->data,
                                                     wr->len, err);
                    }
                    free(wr);
                }
                _tcp_destroy_conn(conn, err);
            } else {
                _tcp_start_close_conn(conn, err);
            }
            return;
        }

        req->offset += (size_t)n;

        if (req->offset == req->len) {
            /* Fully sent — dequeue and notify */
            xylem_queue_dequeue(&conn->write_queue);
            xylem_logd("tcp conn fd=%d sent %zu bytes (complete)",
                       (int)conn->fd, req->len);

            if (conn->handler && conn->handler->on_write_done) {
                conn->handler->on_write_done(conn,
                    req->data, req->len, 0);
            }

            free(req);
        } else {
            /* Partial write — wait for next WR event */
            xylem_logd("tcp conn fd=%d partial write %zd/%zu",
                       (int)conn->fd, n, rem);
            return;
        }
    }

    /* Write queue is now empty */
    if (conn->opts.write_timeout_ms > 0) {
        xylem_loop_stop_timer(&conn->write_timer);
    }

    if (conn->state == XYLEM_TCP_STATE_CLOSING) {
        /* Graceful close: queue drained, finish shutdown */
        xylem_logd("tcp conn fd=%d write queue drained, shutting down",
                   (int)conn->fd);
        shutdown(conn->fd, PLATFORM_SHUT_WR);
        _tcp_destroy_conn(conn, 0);
    } else {
        /* Switch IO interest back to read-only */
        xylem_loop_start_io(&conn->io, PLATFORM_POLLER_RD_OP,
                            _tcp_conn_io_cb);
    }
}

/* Schedule a reconnect attempt with exponential backoff. */
static void _tcp_schedule_reconnect(xylem_tcp_conn_t* conn) {
    _tcp_dial_priv_t* dial = conn->dial;

    if (dial->reconnect_count >= conn->opts.reconnect_max) {
        xylem_logw("tcp conn fd=%d reconnect limit reached (%u)",
                   (int)conn->fd, conn->opts.reconnect_max);
        _tcp_start_close_conn(conn, PLATFORM_SO_ERROR_ETIMEDOUT);
        return;
    }

    /* Exponential backoff: min(500 << count, 30000) ms */
    uint64_t delay = 500ULL << dial->reconnect_count;
    if (delay > 30000) delay = 30000;

    xylem_loop_start_timer(&dial->reconnect_timer,
                           _tcp_reconnect_timer_cb,
                           delay, 0);
    xylem_logi("tcp conn fd=%d scheduling reconnect #%u in %llu ms",
               (int)conn->fd, dial->reconnect_count + 1,
               (unsigned long long)delay);
}

/* Reconnect timer fires: close old socket, create new one, re-dial. */
static void _tcp_reconnect_timer_cb(xylem_loop_t* loop,
                                    xylem_loop_timer_t* timer) {
    (void)loop;
    _tcp_dial_priv_t* dial =
        xylem_list_entry(timer, _tcp_dial_priv_t, reconnect_timer);
    xylem_tcp_conn_t* conn = dial->conn;

    /* Tear down old IO and socket */
    xylem_loop_stop_io(&conn->io);
    platform_socket_close(conn->fd);

    /* Extract host/port from peer_addr */
    char host[INET6_ADDRSTRLEN];
    char port_str[8];
    _tcp_resolve_hostport(&dial->peer_addr, host, sizeof(host),
                          port_str, sizeof(port_str));

    /* Create new socket and attempt connection */
    bool connected = false;
    platform_sock_t fd = platform_socket_dial(host, port_str,
                                              SOCK_STREAM,
                                              &connected, true);

    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        /* Socket creation failed — try again later */
        xylem_logw("tcp reconnect: socket creation failed, retrying");
        dial->reconnect_count++;
        _tcp_schedule_reconnect(conn);
        return;
    }

    conn->fd = fd;
    /**
     * Re-use existing IO handle — just update the fd and reset
     * registration state.  Do NOT call io_init again because that
     * would increment active_count a second time.
     */
    conn->io.sqe.fd     = fd;
    conn->io.registered  = false;
    conn->state = XYLEM_TCP_STATE_CONNECTING;
    dial->reconnect_count++;

    if (connected) {
        _tcp_conn_connected_cb(conn);
    } else {
        /* Connection in progress — wait for WR event */
        xylem_loop_start_io(&conn->io, PLATFORM_POLLER_WR_OP,
                            _tcp_try_connect);

        if (conn->opts.connect_timeout_ms > 0) {
            xylem_loop_start_timer(&dial->connect_timer,
                                   _tcp_connect_timeout_cb,
                                   conn->opts.connect_timeout_ms, 0);
        }
    }
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

    /* Stop connect timer regardless of outcome */
    if (conn->opts.connect_timeout_ms > 0) {
        xylem_loop_stop_timer(&dial->connect_timer);
    }

    if (err == 0) {
        _tcp_conn_connected_cb(conn);
    } else {
        if (conn->opts.reconnect_max > 0 &&
            dial->reconnect_count < conn->opts.reconnect_max) {
            _tcp_schedule_reconnect(conn);
        } else {
            _tcp_destroy_conn(conn, err);
        }
    }
}

static void _tcp_server_io_cb(xylem_loop_t* loop,
                              xylem_loop_io_t* io,
                              platform_poller_op_t revents) {
    (void)loop;
    (void)revents;
    xylem_tcp_server_t* server =
        xylem_list_entry(io, xylem_tcp_server_t, io);

    for (;;) {
        platform_sock_t client_fd =
            platform_socket_accept(server->fd, true);

        if (client_fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK)
                break;
            /* Non-EAGAIN error: log and continue listening */
            xylem_logw("tcp server fd=%d accept error=%d",
                       (int)server->fd, err);
            continue;
        }

        xylem_tcp_conn_t* conn = calloc(1, sizeof(*conn));
        if (!conn) {
            platform_socket_close(client_fd);
            continue;
        }

        conn->loop    = server->loop;
        conn->fd      = client_fd;
        conn->handler = server->handler;
        conn->opts    = server->opts;

        xylem_queue_init(&conn->write_queue);

        /* Initialize IO handle */
        xylem_loop_init_io(server->loop, &conn->io, client_fd);

        /* Initialize timers needed for accepted connections */
        xylem_loop_init_timer(server->loop, &conn->read_timer);
        xylem_loop_init_timer(server->loop, &conn->write_timer);
        xylem_loop_init_timer(server->loop, &conn->heartbeat_timer);

        /* Common setup: state, ringbuf, IO start, timers */
        _tcp_setup_conn(conn);

        /* Add to server's connections list */
        conn->server = server;
        xylem_list_insert_tail(&server->connections,
                               &conn->server_node);

        xylem_logd("tcp server fd=%d accepted conn fd=%d",
                   (int)server->fd, (int)client_fd);

        /* Notify user */
        if (server->handler && server->handler->on_accept) {
            server->handler->on_accept(conn);
        }

        /* User may have called server_close inside on_accept */
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
    if (server->closing) return;

    xylem_logi("tcp server fd=%d closing", (int)server->fd);
    server->closing = true;

    xylem_loop_stop_io(&server->io);
    server->loop->active_count--;
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

    /* Defer free to next loop iteration so close_node stays valid */
    server->free_post.cb = _tcp_server_free_cb;
    xylem_loop_post(server->loop, &server->free_post);
}

void xylem_tcp_close(xylem_tcp_conn_t* conn) {
    if (conn->state == XYLEM_TCP_STATE_CLOSING ||
        conn->state == XYLEM_TCP_STATE_CLOSED)
        return;

    xylem_logd("tcp conn fd=%d graceful close requested",
               (int)conn->fd);
    conn->state = XYLEM_TCP_STATE_CLOSING;

    if (xylem_queue_empty(&conn->write_queue)) {
        /* No pending writes — shutdown and destroy immediately */
        shutdown(conn->fd, PLATFORM_SHUT_WR);
        _tcp_destroy_conn(conn, 0);
    }
    /**
     * Otherwise, _tcp_flush_writes will complete the close when
     * the write queue empties and state is CLOSING.
     */
}

int xylem_tcp_send(xylem_tcp_conn_t* conn, const void* data, size_t len) {
    if (conn->state == XYLEM_TCP_STATE_CLOSING ||
        conn->state == XYLEM_TCP_STATE_CLOSED)
        return -1;

    /* Single allocation: req header + data payload */
    xylem_tcp_write_req_t* req = malloc(sizeof(*req) + len);
    if (!req) return -1;

    req->data   = (char*)req + sizeof(*req);
    req->len    = len;
    req->offset = 0;
    memcpy(req->data, data, len);

    bool was_empty = xylem_queue_empty(&conn->write_queue);
    xylem_queue_enqueue(&conn->write_queue, &req->node);
    xylem_logd("tcp conn fd=%d enqueue write %zu bytes", (int)conn->fd, len);

    /* If queue was empty, switch to RW interest to get WR events */
    if (was_empty) {
        xylem_loop_start_io(&conn->io,
                            PLATFORM_POLLER_RD_OP | PLATFORM_POLLER_WR_OP,
                            _tcp_conn_io_cb);

        /* Start write timeout if configured */
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
    if (!conn) return NULL;

    _tcp_dial_priv_t* dial = calloc(1, sizeof(*dial));
    if (!dial) {
        free(conn);
        return NULL;
    }

    dial->conn            = conn;
    dial->peer_addr       = *addr;
    dial->reconnect_count = 0;
    conn->dial            = dial;

    /* Copy options (use defaults if NULL) */
    if (opts) {
        conn->opts = *opts;
    }

    if (conn->opts.read_buf_size == 0) {
        conn->opts.read_buf_size = XYLEM_TCP_DEFAULT_READ_BUF_SIZE;
    }

    conn->loop    = loop;
    conn->handler = handler;
    conn->state   = XYLEM_TCP_STATE_CONNECTING;

    xylem_queue_init(&conn->write_queue);

    /* Extract host/port from addr for platform_socket_dial */
    char host[INET6_ADDRSTRLEN];
    char port_str[8];
    _tcp_resolve_hostport(addr, host, sizeof(host),
                          port_str, sizeof(port_str));

    bool connected = false;
    platform_sock_t fd = platform_socket_dial(host, port_str,
                                              SOCK_STREAM,
                                              &connected, true);

    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        free(dial);
        free(conn);
        xylem_loge("tcp dial: socket creation failed for %s:%s",
                   host, port_str);
        return NULL;
    }

    conn->fd = fd;
    xylem_logi("tcp dial fd=%d to %s:%s", (int)fd, host, port_str);

    /* Initialize IO handle */
    xylem_loop_init_io(loop, &conn->io, conn->fd);

    /* Initialize timers */
    xylem_loop_init_timer(loop, &dial->connect_timer);
    xylem_loop_init_timer(loop, &conn->read_timer);
    xylem_loop_init_timer(loop, &conn->write_timer);
    xylem_loop_init_timer(loop, &conn->heartbeat_timer);
    xylem_loop_init_timer(loop, &dial->reconnect_timer);

    if (connected) {
        _tcp_conn_connected_cb(conn);
    } else {
        /* Connection in progress — wait for WR event */
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
    if (!server) return NULL;

    /* Copy options (use defaults if NULL) */
    if (opts) {
        server->opts = *opts;
    }

    if (server->opts.read_buf_size == 0) {
        server->opts.read_buf_size = XYLEM_TCP_DEFAULT_READ_BUF_SIZE;
    }

    server->loop    = loop;
    server->handler = handler;
    server->closing = false;

    xylem_list_init(&server->connections);

    /* Extract host/port from addr */
    char host[INET6_ADDRSTRLEN];
    char port_str[8];
    _tcp_resolve_hostport(addr, host, sizeof(host),
                          port_str, sizeof(port_str));

    /* Create listening socket */
    platform_sock_t fd = platform_socket_listen(host, port_str,
                                                SOCK_STREAM, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        free(server);
        xylem_loge("tcp listen: socket creation failed for %s:%s",
                   host, port_str);
        return NULL;
    }

    server->fd = fd;

    /* Register with event loop */
    xylem_loop_init_io(loop, &server->io, server->fd);
    xylem_loop_start_io(&server->io, PLATFORM_POLLER_RD_OP,
                        _tcp_server_io_cb);

    xylem_logi("tcp server fd=%d listening on %s:%s",
               (int)fd, host, port_str);
    return server;
}
