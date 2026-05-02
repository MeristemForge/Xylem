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

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TCP_DEFAULT_READ_BUF_SIZE 65536

#define TCP_ERR_NONE        0
#define TCP_ERR_INTERNAL    (-1)

#define TCP_WRITE_SUCCESS   0
#define TCP_WRITE_FAILED    (-1)

#define TCP_ERRMSG_NONE     "none"
#define TCP_ERRMSG_INTERNAL "internal error"

typedef enum {
    TCP_STATE_CONNECTING,
    TCP_STATE_CONNECTED,
    TCP_STATE_CLOSING,
    TCP_STATE_CLOSED,
} _tcp_state_t;

typedef struct _tcp_write_req_s {
    xylem_queue_node_t node;
    const void*        data;
    size_t             len;
    size_t             offset;
} _tcp_write_req_t;

typedef struct _tcp_write_done_s {
    xylem_tcp_conn_t* tcp;
    const void*       data;
    size_t            len;
} _tcp_write_done_t;

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
    xylem_tcp_conn_t*     tcp;
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
    _Atomic int32_t       refcount;
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
 * < 0 on error. On success, *frame_out points into tcp->read_buf
 * (zero-copy) and *frame_len_out is the payload length.
 * The pointer is valid until the next recv or compact.
 */
static ssize_t _tcp_extract_frame(xylem_tcp_conn_t* tcp,
                                  void** frame_out,
                                  size_t* frame_len_out) {
    uint8_t* data  = tcp->read_buf;
    size_t   avail = tcp->read_len;

    if (avail == 0) {
        return 0;
    }

    switch (tcp->opts.framing.type) {

    case XYLEM_TCP_FRAME_NONE: {
        *frame_out     = data;
        *frame_len_out = avail;
        return (ssize_t)avail;
    }

    case XYLEM_TCP_FRAME_FIXED: {
        size_t fsz = tcp->opts.framing.fixed.frame_size;
        if (fsz == 0) {
            xylem_loge("tcp conn fd=%d frame_fixed: frame_size=0",
                       (int)tcp->fd);
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
        uint32_t hdr_sz  = tcp->opts.framing.length.header_size;
        uint32_t len_off = tcp->opts.framing.length.field_offset;
        uint32_t len_sz  = tcp->opts.framing.length.field_size;
        int32_t  adj     = tcp->opts.framing.length.adjustment;

        if (avail < hdr_sz) {
            return 0;
        }

        uint32_t effective_hdr = hdr_sz;
        uint64_t payload_len = 0;

        if (tcp->opts.framing.length.coding == XYLEM_TCP_LENGTH_FIXEDINT) {
            if (len_sz == 0 || len_sz > 8) {
                xylem_loge("tcp conn fd=%d frame_length: invalid field_size=%u",
                           (int)tcp->fd, len_sz);
                return -1;
            }
            if (len_off > avail || len_sz > avail - len_off) {
                return 0;
            }

            if (tcp->opts.framing.length.field_big_endian) {
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
            if (hdr_sz + varint_bytes < len_sz) {
                xylem_loge("tcp conn fd=%d frame_length: varint underflow",
                           (int)tcp->fd);
                return -1;
            }
            effective_hdr = hdr_sz + varint_bytes - len_sz;
        }

        if (payload_len > (uint64_t)INT32_MAX) {
            xylem_loge("tcp conn fd=%d frame_length: payload_len overflow",
                       (int)tcp->fd);
            return -1;
        }

        int64_t frame_size = (int64_t)effective_hdr + (int64_t)payload_len +
                             (int64_t)adj;
        if (frame_size <= 0 || (uint64_t)frame_size <= effective_hdr) {
            xylem_loge("tcp conn fd=%d frame_length: frame_size=%" PRId64
                       " invalid", (int)tcp->fd, frame_size);
            return -1;
        }

        size_t total = (size_t)frame_size;
        if (avail < total) {
            return 0;
        }

        *frame_out     = data + effective_hdr;
        *frame_len_out = total - effective_hdr;
        return (ssize_t)total;
    }

    case XYLEM_TCP_FRAME_DELIM: {
        const char* delim     = tcp->opts.framing.delim.delim;
        size_t      delim_len = tcp->opts.framing.delim.delim_len;
        if (!delim || delim_len == 0) {
            xylem_loge("tcp conn fd=%d frame_delim: delim is NULL or empty",
                       (int)tcp->fd);
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
        if (!tcp->opts.framing.custom.parse) {
            xylem_loge("tcp conn fd=%d frame_custom: parse is NULL",
                       (int)tcp->fd);
            return -1;
        }

        int rc = tcp->opts.framing.custom.parse(data, avail);

        if (rc > 0) {
            if ((size_t)rc > avail) {
                xylem_loge("tcp conn fd=%d frame_custom: parse returned %d"
                           " > avail %zu", (int)tcp->fd, rc, avail);
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
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;

    xylem_logw("tcp conn fd=%d read timeout", (int)tcp->fd);
    if (tcp->handler && tcp->handler->on_timeout) {
        tcp->handler->on_timeout(tcp, XYLEM_TCP_TIMEOUT_READ);
    }
}

static void _tcp_write_timeout_cb(xylem_loop_t* loop,
                                  xylem_loop_timer_t* timer,
                                  void* ud) {
    (void)loop;
    (void)timer;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;

    xylem_logw("tcp conn fd=%d write timeout", (int)tcp->fd);
    if (tcp->handler && tcp->handler->on_timeout) {
        tcp->handler->on_timeout(tcp, XYLEM_TCP_TIMEOUT_WRITE);
    }
}

static void _tcp_conn_destroy(xylem_tcp_conn_t* tcp, int err);
static void _tcp_reconnect_start_timer(xylem_tcp_conn_t* tcp,
                                       xylem_loop_timer_fn_t cb);

static void _tcp_connect_timeout_cb(xylem_loop_t* loop,
                                    xylem_loop_timer_t* timer,
                                    void* ud) {
    (void)loop;
    (void)timer;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;

    xylem_logw("tcp conn fd=%d connect timeout", (int)tcp->fd);
    if (tcp->handler && tcp->handler->on_timeout) {
        tcp->handler->on_timeout(tcp, XYLEM_TCP_TIMEOUT_CONNECT);
    }

    /* User may have called xylem_tcp_close inside on_timeout. */
    if (tcp->state == TCP_STATE_CLOSED) {
        return;
    }

    /* Stop watching the stale socket and attempt reconnect or close. */
    xylem_loop_stop_io(tcp->io);

    _tcp_dial_priv_t* dial = tcp->dial;
    if (dial && tcp->opts.reconnect_max > 0 &&
        dial->reconnect_count < tcp->opts.reconnect_max) {
        _tcp_reconnect_start_timer(tcp, dial->reconnect_cb);
    } else {
        _tcp_conn_destroy(tcp, PLATFORM_SO_ERROR_ETIMEDOUT);
    }
}

static void _tcp_heartbeat_timeout_cb(xylem_loop_t* loop,
                                      xylem_loop_timer_t* timer,
                                      void* ud) {
    (void)loop;
    (void)timer;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;

    xylem_logw("tcp conn fd=%d heartbeat miss", (int)tcp->fd);
    if (tcp->handler && tcp->handler->on_heartbeat_miss) {
        tcp->handler->on_heartbeat_miss(tcp);
    }
}

/* Decrement refcount; free the connection when it reaches zero. */
static void _tcp_conn_decref(xylem_tcp_conn_t* tcp) {
    if (atomic_fetch_sub(&tcp->refcount, 1) == 1) {
        free(tcp);
    }
}

/**
 * Post callback: decrement refcount after the current iteration.
 * This ensures the tcp pointer remains valid for the remainder
 * of the current callback chain (e.g. IO batch, timer batch).
 */
static void _tcp_post_conn_free_cb(xylem_loop_t* loop,
                              xylem_loop_post_t* req,
                              void* ud) {
    (void)loop;
    (void)req;
    _tcp_conn_decref((xylem_tcp_conn_t*)ud);
}

static void _tcp_conn_destroy(xylem_tcp_conn_t* tcp, int err) {
    tcp->state = TCP_STATE_CLOSED;
    xylem_logd("tcp conn fd=%d destroy err=%d (%s)",
               (int)tcp->fd, err,
               err ? platform_socket_tostring(err) : TCP_ERRMSG_NONE);

    if (tcp->server) {
        xylem_list_remove(&tcp->server->connections, &tcp->server_node);
        tcp->server = NULL;
    }

    if (tcp->dial) {
        xylem_loop_destroy_timer(tcp->dial->connect_timer);
        tcp->dial->connect_timer = NULL;
        xylem_loop_destroy_timer(tcp->dial->reconnect_timer);
        tcp->dial->reconnect_timer = NULL;
    }

    xylem_loop_destroy_timer(tcp->read_timer);
    tcp->read_timer = NULL;
    xylem_loop_destroy_timer(tcp->write_timer);
    tcp->write_timer = NULL;
    xylem_loop_destroy_timer(tcp->heartbeat_timer);
    tcp->heartbeat_timer = NULL;

    xylem_loop_destroy_io(tcp->io);
    tcp->io = NULL;
    platform_socket_close(tcp->fd);

    free(tcp->read_buf);
    tcp->read_buf = NULL;

    if (tcp->dial) {
        free(tcp->dial);
        tcp->dial = NULL;
    }

    if (tcp->handler && tcp->handler->on_close) {
        const char* errmsg;
        if (err == TCP_ERR_NONE) {
            errmsg = TCP_ERRMSG_NONE;
        } else if (err == TCP_ERR_INTERNAL) {
            errmsg = TCP_ERRMSG_INTERNAL;
        } else {
            errmsg = platform_socket_tostring(err);
        }
        tcp->handler->on_close(tcp, err, errmsg);
    }

    xylem_loop_post(tcp->loop, _tcp_post_conn_free_cb, tcp);
}

static void _tcp_conn_close(xylem_tcp_conn_t* tcp, int err) {
    if (tcp->state == TCP_STATE_CLOSED) {
        return;
    }

    xylem_logd("tcp conn fd=%d start_close err=%d (%s)",
               (int)tcp->fd, err,
               err ? platform_socket_tostring(err) : TCP_ERRMSG_NONE);
    tcp->state = TCP_STATE_CLOSING;

    while (!xylem_queue_empty(&tcp->write_queue)) {
        xylem_queue_node_t* node =
            xylem_queue_dequeue(&tcp->write_queue);
        _tcp_write_req_t* req =
            xylem_queue_entry(node, _tcp_write_req_t, node);

        if (tcp->handler && tcp->handler->on_write_done) {
            tcp->handler->on_write_done(tcp, req->data, req->len, TCP_WRITE_FAILED);
        }

        free(req);
    }

    _tcp_conn_destroy(tcp, err);
}

static void _tcp_conn_readable_cb(xylem_tcp_conn_t* tcp) {
    for (;;) {
        size_t space = tcp->read_cap - tcp->read_len;
        if (space == 0) {
            xylem_logw("tcp conn fd=%d read buffer full, closing",
                       (int)tcp->fd);
            _tcp_conn_close(tcp, TCP_ERR_INTERNAL);
            return;
        }

        ssize_t nread = platform_socket_recv(
            tcp->fd, tcp->read_buf + tcp->read_len, (int)space);

        if (nread == 0) {
            xylem_logi("tcp conn fd=%d peer closed", (int)tcp->fd);
            _tcp_conn_close(tcp, TCP_ERR_NONE);
            return;
        }

        if (nread < 0) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                break;
            }
            xylem_loge("tcp conn fd=%d recv error=%d (%s)",
                       (int)tcp->fd, err,
                       platform_socket_tostring(err));
            _tcp_conn_close(tcp, err);
            return;
        }

        tcp->read_len += (size_t)nread;
        xylem_logd("tcp conn fd=%d recv %zd bytes",
                   (int)tcp->fd, nread);

        for (;;) {
            void*  frame_data = NULL;
            size_t frame_len  = 0;
            ssize_t rc = _tcp_extract_frame(tcp, &frame_data, &frame_len);

            if (rc > 0) {
                if (tcp->handler && tcp->handler->on_read) {
                    tcp->handler->on_read(tcp, frame_data, frame_len);
                }

                /* User may have closed or destroyed via send failure. */
                if (tcp->state != TCP_STATE_CONNECTED) {
                    return;
                }

                /* Compact so next extract sees correct data. */
                tcp->read_len -= (size_t)rc;
                if (tcp->read_len > 0) {
                    memmove(tcp->read_buf,
                            tcp->read_buf + (size_t)rc,
                            tcp->read_len);
                }
            } else if (rc == 0) {
                break;
            } else {
                _tcp_conn_close(tcp, TCP_ERR_INTERNAL);
                return;
            }
        }

        if ((size_t)nread < space) {
            break;
        }
    }

    if (tcp->opts.heartbeat_ms > 0 && tcp->heartbeat_timer) {
        xylem_loop_reset_timer(tcp->heartbeat_timer,
                               tcp->opts.heartbeat_ms);
    }

    if (tcp->opts.read_timeout_ms > 0 && tcp->read_timer) {
        xylem_loop_reset_timer(tcp->read_timer,
                               tcp->opts.read_timeout_ms);
    }
}

static void _tcp_conn_io_cb(xylem_loop_t* loop,
                            xylem_loop_io_t* io,
                            xylem_poller_op_t revents,
                            void* ud);

static void _tcp_flush_writes(xylem_tcp_conn_t* tcp) {
    while (!xylem_queue_empty(&tcp->write_queue)) {
        xylem_queue_node_t* front =
            xylem_queue_front(&tcp->write_queue);
        _tcp_write_req_t* req =
            xylem_queue_entry(front, _tcp_write_req_t, node);

        const char* ptr = (const char*)req->data + req->offset;
        size_t      rem = req->len - req->offset;

        ssize_t n = platform_socket_send(tcp->fd, ptr, (int)rem);

        if (n < 0) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                return;
            }

            xylem_loge("tcp conn fd=%d send error=%d (%s)",
                       (int)tcp->fd, err,
                       platform_socket_tostring(err));
            _tcp_conn_close(tcp, err);
            return;
        }

        req->offset += (size_t)n;

        if (req->offset == req->len) {
            xylem_queue_dequeue(&tcp->write_queue);
            xylem_logd("tcp conn fd=%d sent %zu bytes (complete)",
                       (int)tcp->fd, req->len);

            if (tcp->handler && tcp->handler->on_write_done) {
                tcp->handler->on_write_done(tcp,
                    req->data, req->len, TCP_WRITE_SUCCESS);
            }

            free(req);

            /* User may have closed or destroyed via send failure. */
            if (tcp->state != TCP_STATE_CONNECTED) {
                return;
            }

            if (tcp->opts.write_timeout_ms > 0 && tcp->write_timer &&
                !xylem_queue_empty(&tcp->write_queue)) {
                xylem_loop_reset_timer(tcp->write_timer,
                                       tcp->opts.write_timeout_ms);
            }
        } else {
            xylem_logd("tcp conn fd=%d partial write %zd/%zu",
                       (int)tcp->fd, n, rem);
            return;
        }
    }

    if (tcp->opts.write_timeout_ms > 0 && tcp->write_timer) {
        xylem_loop_stop_timer(tcp->write_timer);
    }

    /**
     * Write queue fully drained while CLOSING: the graceful shutdown
     * sequence can now complete with shutdown(SHUT_WR) + destroy.
     */
    if (tcp->state == TCP_STATE_CLOSING) {
        xylem_logd("tcp conn fd=%d write queue drained, shutting down",
                   (int)tcp->fd);
        shutdown(tcp->fd, PLATFORM_SHUT_WR);
        _tcp_conn_destroy(tcp, TCP_ERR_NONE);
    }
}

static void _tcp_conn_io_cb(xylem_loop_t* loop,
                            xylem_loop_io_t* io,
                            xylem_poller_op_t revents,
                            void* ud) {
    (void)loop;
    (void)io;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;

    if (revents & XYLEM_POLLER_RD_OP) {
        _tcp_conn_readable_cb(tcp);
    }

    /**
     * CLOSING is intentionally allowed through -- flush_writes needs to
     * drain the write queue before the connection is fully torn down.
     */
    if (tcp->state == TCP_STATE_CLOSED) {
        return;
    }

    if (revents & XYLEM_POLLER_WR_OP) {
        _tcp_flush_writes(tcp);

        if (tcp->state == TCP_STATE_CONNECTED &&
            xylem_queue_empty(&tcp->write_queue)) {
            xylem_loop_start_io(tcp->io, XYLEM_POLLER_RD_OP,
                                _tcp_conn_io_cb, tcp);
        }
    }
}

/**
 * Common setup for a newly connected socket: allocate read buffer,
 * start IO, start heartbeat/read timers. Does NOT call any handler
 * callback.
 */
static int _tcp_conn_setup(xylem_tcp_conn_t* tcp) {
    tcp->read_buf = (uint8_t*)malloc(tcp->opts.read_buf_size);
    if (!tcp->read_buf) {
        return -1;
    }

    if (xylem_loop_start_io(tcp->io, XYLEM_POLLER_RD_OP,
                            _tcp_conn_io_cb, tcp) != 0) {
        free(tcp->read_buf);
        tcp->read_buf = NULL;
        return -1;
    }

    tcp->state = TCP_STATE_CONNECTED;
    tcp->read_len = 0;
    tcp->read_cap = tcp->opts.read_buf_size;

    if (tcp->opts.heartbeat_ms > 0) {
        if (!tcp->heartbeat_timer) {
            tcp->heartbeat_timer =
                xylem_loop_create_timer(tcp->loop);
        }
        if (tcp->heartbeat_timer) {
            xylem_loop_start_timer(tcp->heartbeat_timer,
                                   _tcp_heartbeat_timeout_cb,
                                   tcp, tcp->opts.heartbeat_ms,
                                   tcp->opts.heartbeat_ms);
        }
    }

    if (tcp->opts.read_timeout_ms > 0) {
        if (!tcp->read_timer) {
            tcp->read_timer =
                xylem_loop_create_timer(tcp->loop);
        }
        if (tcp->read_timer) {
            xylem_loop_start_timer(tcp->read_timer,
                                   _tcp_read_timeout_cb,
                                   tcp, tcp->opts.read_timeout_ms, 0);
        }
    }

    if (tcp->opts.write_timeout_ms > 0) {
        if (!tcp->write_timer) {
            tcp->write_timer =
                xylem_loop_create_timer(tcp->loop);
        }
    }

    return 0;
}

/**
 * When non-blocking connect succeeds immediately inside xylem_tcp_dial,
 * on_connect would fire before dial returns -- the caller has no chance to
 * call set_userdata yet, so the callback sees NULL userdata.  Deferring to
 * the next loop iteration guarantees dial returns first.
 */
static void _tcp_post_connect_cb(xylem_loop_t* loop,
                                 xylem_loop_post_t* req,
                                 void* ud) {
    (void)loop;
    (void)req;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;
    /**
     * Between post and fire: IO readable may have triggered destroy
     * (peer closed immediately), or user may have called xylem_tcp_close
     * after xylem_tcp_dial returned.  Both make on_connect stale.
     */
    if (tcp->state == TCP_STATE_CLOSED ||
        tcp->state == TCP_STATE_CLOSING) {
        return;
    }
    if (tcp->handler && tcp->handler->on_connect) {
        tcp->handler->on_connect(tcp);
    }
}

static void _tcp_connected_cb(xylem_tcp_conn_t* tcp) {
    if (_tcp_conn_setup(tcp) != 0) {
        xylem_loge("tcp conn fd=%d setup failed", (int)tcp->fd);
        _tcp_conn_close(tcp, TCP_ERR_INTERNAL);
        return;
    }
    xylem_logi("tcp conn fd=%d connected", (int)tcp->fd);

    if (tcp->handler && tcp->handler->on_connect) {
        tcp->handler->on_connect(tcp);
    }
}

static void _tcp_try_connect(xylem_loop_t* loop,
                             xylem_loop_io_t* io,
                             xylem_poller_op_t revents,
                             void* ud);

/* Shared reconnect logic: check limit, compute backoff, start timer. */
static void _tcp_reconnect_start_timer(xylem_tcp_conn_t* tcp,
                                       xylem_loop_timer_fn_t cb) {
    _tcp_dial_priv_t* dial = tcp->dial;

    if (dial->reconnect_count >= tcp->opts.reconnect_max) {
        xylem_logw("tcp conn fd=%d reconnect limit reached (%u)",
                   (int)tcp->fd, tcp->opts.reconnect_max);
        _tcp_conn_close(tcp, PLATFORM_SO_ERROR_ETIMEDOUT);
        return;
    }

    uint64_t delay = 500ULL << (dial->reconnect_count < 16
                                ? dial->reconnect_count : 16);
    if (delay > 30000) {
        delay = 30000;
    }

    xylem_loop_start_timer(dial->reconnect_timer, cb, tcp, delay, 0);
    xylem_logi("tcp conn fd=%d scheduling reconnect #%u in %" PRIu64 " ms",
               (int)tcp->fd, dial->reconnect_count + 1,
               delay);
}

static void _tcp_try_connect(xylem_loop_t* loop,
                             xylem_loop_io_t* io,
                             xylem_poller_op_t revents,
                             void* ud) {
    (void)loop;
    (void)io;
    (void)revents;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;
    _tcp_dial_priv_t* dial = tcp->dial;

    int err    = 0;
    socklen_t errlen = sizeof(err);

    if (getsockopt(tcp->fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen) != 0) {
        err = platform_socket_get_lasterror();
        if (err == 0) {
            err = TCP_ERR_INTERNAL;
        }
    }

    if (tcp->opts.connect_timeout_ms > 0 && dial->connect_timer) {
        xylem_loop_stop_timer(dial->connect_timer);
    }

    if (err == 0) {
        dial->reconnect_count = 0;
        _tcp_connected_cb(tcp);
    } else {
        xylem_loop_stop_io(tcp->io);
        if (tcp->opts.reconnect_max > 0 &&
            dial->reconnect_count < tcp->opts.reconnect_max) {
            _tcp_reconnect_start_timer(tcp, dial->reconnect_cb);
        } else {
            _tcp_conn_destroy(tcp, err);
        }
    }
}

static void _tcp_reconnect_timeout_cb(xylem_loop_t* loop,
                                      xylem_loop_timer_t* timer,
                                      void* ud) {
    (void)loop;
    (void)timer;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;
    _tcp_dial_priv_t* dial = tcp->dial;

    xylem_loop_stop_io(tcp->io);
    platform_socket_close(tcp->fd);
    tcp->fd = PLATFORM_SO_ERROR_INVALID_SOCKET;
    dial->reconnect_count++;

    bool connected = false;
    platform_sock_t fd = platform_socket_dial(dial->host, dial->port_str,
                                              SOCK_STREAM,
                                              &connected, true);

    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_logw("tcp reconnect: socket creation failed for %s:%s",
                   dial->host, dial->port_str);
        _tcp_reconnect_start_timer(tcp, _tcp_reconnect_timeout_cb);
        return;
    }

    tcp->fd = fd;
    xylem_loop_destroy_io(tcp->io);
    tcp->io = xylem_loop_create_io(tcp->loop, fd);

    if (!tcp->io) {
        xylem_logw("tcp reconnect fd=%d: io creation failed for %s:%s",
                   (int)fd, dial->host, dial->port_str);
        platform_socket_close(fd);
        tcp->fd = PLATFORM_SO_ERROR_INVALID_SOCKET;
        _tcp_reconnect_start_timer(tcp, _tcp_reconnect_timeout_cb);
        return;
    }

    tcp->state = TCP_STATE_CONNECTING;

    if (tcp->opts.disable_mss_clamp) {
        platform_socket_enable_mss_clamp(fd, false);
    }

    if (connected) {
        dial->reconnect_count = 0;
        _tcp_connected_cb(tcp);
    } else {
        xylem_loop_start_io(tcp->io, XYLEM_POLLER_WR_OP,
                            _tcp_try_connect, tcp);

        if (tcp->opts.connect_timeout_ms > 0 && dial->connect_timer) {
            xylem_loop_start_timer(dial->connect_timer,
                                   _tcp_connect_timeout_cb,
                                   tcp, tcp->opts.connect_timeout_ms, 0);
        }
    }
}

static void _tcp_server_io_cb(xylem_loop_t* loop,
                              xylem_loop_io_t* io,
                              xylem_poller_op_t revents,
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

        xylem_tcp_conn_t* tcp =
            (xylem_tcp_conn_t*)calloc(1, sizeof(xylem_tcp_conn_t));
        if (!tcp) {
            xylem_logw("tcp server fd=%d accept: conn alloc failed",
                       (int)server->fd);
            platform_socket_close(client_fd);
            continue;
        }

        tcp->loop    = loop;
        tcp->fd      = client_fd;
        tcp->handler = server->handler;
        tcp->opts    = server->opts;
        atomic_store(&tcp->refcount, 1);

        /* TCP_NODELAY inheritance is platform-dependent; set explicitly. */
        platform_socket_enable_nodelay(client_fd, true);

        xylem_queue_init(&tcp->write_queue);

        tcp->io = xylem_loop_create_io(loop, client_fd);
        if (!tcp->io) {
            xylem_logw("tcp server fd=%d accept: io creation failed for fd=%d",
                       (int)server->fd, (int)client_fd);
            platform_socket_close(client_fd);
            free(tcp);
            continue;
        }

        if (_tcp_conn_setup(tcp) != 0) {
            xylem_logw("tcp server fd=%d accept: setup failed for fd=%d",
                       (int)server->fd, (int)client_fd);
            xylem_loop_destroy_io(tcp->io);
            platform_socket_close(client_fd);
            free(tcp);
            continue;
        }

        tcp->server = server;
        xylem_list_insert_tail(&server->connections,
                               &tcp->server_node);

        socklen_t peer_len = sizeof(tcp->peer_addr.storage);
        if (getpeername(client_fd, (struct sockaddr*)&tcp->peer_addr.storage,
                        &peer_len) != 0) {
            xylem_logw("tcp server fd=%d: getpeername failed for conn fd=%d",
                       (int)server->fd, (int)client_fd);
        }

        xylem_logi("tcp server fd=%d accepted conn fd=%d",
                   (int)server->fd, (int)client_fd);

        if (server->handler && server->handler->on_accept) {
            server->handler->on_accept(server, tcp);
        }

        if (server->closing) {
            break;
        }
    }
}

/* Post callback: free a server after the current iteration. */
static void _tcp_post_server_free_cb(xylem_loop_t* loop,
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
        xylem_tcp_conn_t* tcp =
            xylem_list_entry(node, xylem_tcp_conn_t, server_node);
        xylem_list_remove(&server->connections, node);
        tcp->server = NULL;
        xylem_tcp_close(tcp);
    }

    xylem_loop_post(server->loop, _tcp_post_server_free_cb, server);
}

static void _tcp_post_graceful_close_cb(xylem_loop_t* loop,
                                        xylem_loop_post_t* req,
                                        void* ud) {
    (void)loop;
    (void)req;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;
    if (tcp->state == TCP_STATE_CLOSED) {
        return;
    }
    _tcp_conn_destroy(tcp, TCP_ERR_NONE);
}

void xylem_tcp_close(xylem_tcp_conn_t* tcp) {
    if (tcp->state == TCP_STATE_CLOSING ||
        tcp->state == TCP_STATE_CLOSED) {
        return;
    }

    xylem_logi("tcp conn fd=%d graceful close requested",
               (int)tcp->fd);

    if (tcp->state == TCP_STATE_CONNECTING) {
        _tcp_conn_destroy(tcp, TCP_ERR_NONE);
        return;
    }

    tcp->state = TCP_STATE_CLOSING;

    if (xylem_queue_empty(&tcp->write_queue)) {
        shutdown(tcp->fd, PLATFORM_SHUT_WR);
        /**
         * Defer destroy via post instead of calling _tcp_conn_destroy
         * directly.  _tcp_direct_write posts on_write_done to the same
         * MPSC queue; if we destroy now, those posts see state==CLOSED
         * and get silently dropped -- the user never receives write_done
         * for data that was already sent.  Posting destroy ensures FIFO
         * ordering: pending write_done callbacks fire first, then destroy.
         */
        xylem_loop_post(tcp->loop, _tcp_post_graceful_close_cb, tcp);
    } else {
        /* Only flush writes; stop reading to avoid premature close on peer FIN. */
        xylem_loop_start_io(tcp->io, XYLEM_POLLER_WR_OP,
                            _tcp_conn_io_cb, tcp);
    }
}

static void _tcp_post_write_done_cb(xylem_loop_t* loop,
                                    xylem_loop_post_t* req,
                                    void* ud) {
    (void)loop;
    (void)req;
    _tcp_write_done_t* wd = (_tcp_write_done_t*)ud;

    /* Connection may have been destroyed between post and fire. */
    if (wd->tcp->state == TCP_STATE_CLOSED) {
        free(wd);
        return;
    }

    if (wd->tcp->handler && wd->tcp->handler->on_write_done) {
        wd->tcp->handler->on_write_done(wd->tcp, wd->data, wd->len, TCP_WRITE_SUCCESS);
    }

    free(wd);
}

/* Enqueue a write request and arm IO/timer. */
static int _tcp_enqueue_write(xylem_tcp_conn_t* tcp,
                              const void* data,
                              size_t len,
                              size_t offset) {
    _tcp_write_req_t* req =
        (_tcp_write_req_t*)malloc(sizeof(*req));
    if (!req) {
        return -1;
    }

    req->data   = data;
    req->len    = len;
    req->offset = offset;

    bool was_empty = xylem_queue_empty(&tcp->write_queue);
    xylem_queue_enqueue(&tcp->write_queue, &req->node);
    xylem_logd("tcp conn fd=%d enqueue write %zu bytes",
               (int)tcp->fd, len);

    if (was_empty) {
        xylem_loop_start_io(tcp->io,
                            XYLEM_POLLER_RD_OP | XYLEM_POLLER_WR_OP,
                            _tcp_conn_io_cb, tcp);

        if (tcp->opts.write_timeout_ms > 0 && tcp->write_timer) {
            xylem_loop_start_timer(tcp->write_timer,
                                   _tcp_write_timeout_cb,
                                   tcp, tcp->opts.write_timeout_ms, 0);
        }
    }

    return 0;
}

/* Try direct send, enqueue remainder if incomplete. */
static int _tcp_direct_write(xylem_tcp_conn_t* tcp,
                             const void* data,
                             size_t len) {
    const char* ptr = (const char*)data;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = platform_socket_send(tcp->fd, ptr, (int)remaining);
        if (n > 0) {
            ptr += n;
            remaining -= (size_t)n;
        } else {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                break;
            }
            _tcp_conn_close(tcp, err);
            return -1;
        }
    }

    if (remaining == 0) {
        if (tcp->handler && tcp->handler->on_write_done) {
            _tcp_write_done_t* wd =
                (_tcp_write_done_t*)malloc(sizeof(*wd));
            if (!wd) {
                _tcp_conn_close(tcp, TCP_ERR_INTERNAL);
                return -1;
            }
            wd->tcp  = tcp;
            wd->data = data;
            wd->len  = len;
            if (xylem_loop_post(tcp->loop, _tcp_post_write_done_cb, wd) != 0) {
                free(wd);
                _tcp_conn_close(tcp, TCP_ERR_INTERNAL);
                return -1;
            }
        }
        return 0;
    }

    return _tcp_enqueue_write(tcp, data, len, len - remaining);
}

static int _tcp_process_write(xylem_tcp_conn_t* tcp,
                              const void* data,
                              size_t len) {
    if (xylem_queue_empty(&tcp->write_queue)) {
        return _tcp_direct_write(tcp, data, len);
    }
    return _tcp_enqueue_write(tcp, data, len, 0);
}

int xylem_tcp_send(xylem_tcp_conn_t* tcp, const void* data, size_t len) {
    if (tcp->state != TCP_STATE_CONNECTED) {
        xylem_logd("tcp conn fd=%d send rejected (state=%d)",
                   (int)tcp->fd,
                   (int)tcp->state);
        return -1;
    }

    if (!data || len == 0) {
        return 0;
    }

    return _tcp_process_write(tcp, data, len);
}

const xylem_addr_t* xylem_tcp_get_peer_addr(xylem_tcp_conn_t* tcp) {
    return &tcp->peer_addr;
}

xylem_loop_t* xylem_tcp_get_loop(xylem_tcp_conn_t* tcp) {
    return tcp->loop;
}

void* xylem_tcp_get_userdata(xylem_tcp_conn_t* tcp) {
    return tcp->userdata;
}

void xylem_tcp_set_userdata(xylem_tcp_conn_t* tcp, void* ud) {
    tcp->userdata = ud;
}

/**
 * Roll back a partially initialised dial connection.
 * Each field is NULL-safe: calloc zeroes everything, so only
 * resources that were actually created get released.
 */
static void _tcp_dial_cleanup(xylem_tcp_conn_t* tcp,
                              _tcp_dial_priv_t* dial) {
    if (dial->connect_timer) {
        xylem_loop_destroy_timer(dial->connect_timer);
    }
    if (dial->reconnect_timer) {
        xylem_loop_destroy_timer(dial->reconnect_timer);
    }
    if (tcp->io) {
        xylem_loop_destroy_io(tcp->io);
    }
    if (tcp->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(tcp->fd);
    }
    free(dial);
    free(tcp);
}

xylem_tcp_conn_t* xylem_tcp_dial(xylem_loop_t* loop,
                                 xylem_addr_t* addr,
                                 xylem_tcp_handler_t* handler,
                                 xylem_tcp_opts_t* opts) {
    xylem_tcp_conn_t* tcp =
        (xylem_tcp_conn_t*)calloc(1, sizeof(xylem_tcp_conn_t));
    if (!tcp) {
        return NULL;
    }
    tcp->fd = PLATFORM_SO_ERROR_INVALID_SOCKET;

    _tcp_dial_priv_t* dial =
        (_tcp_dial_priv_t*)calloc(1, sizeof(_tcp_dial_priv_t));
    if (!dial) {
        free(tcp);
        return NULL;
    }

    dial->tcp            = tcp;
    dial->peer_addr       = *addr;
    dial->reconnect_count = 0;
    dial->reconnect_cb    = _tcp_reconnect_timeout_cb;
    tcp->dial            = dial;
    tcp->peer_addr       = *addr;

    if (opts) {
        tcp->opts = *opts;
    }

    if (tcp->opts.read_buf_size == 0) {
        tcp->opts.read_buf_size = TCP_DEFAULT_READ_BUF_SIZE;
    }

    tcp->loop    = loop;
    tcp->handler = handler;
    tcp->state = TCP_STATE_CONNECTING;
    atomic_store(&tcp->refcount, 1);

    xylem_queue_init(&tcp->write_queue);

    _tcp_resolve_hostport(addr, dial->host, sizeof(dial->host),
                          dial->port_str, sizeof(dial->port_str));

    bool connected = false;
    platform_sock_t fd = platform_socket_dial(dial->host, dial->port_str,
                                              SOCK_STREAM,
                                              &connected, true);

    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("tcp dial: socket creation failed for %s:%s",
                   dial->host, dial->port_str);
        _tcp_dial_cleanup(tcp, dial);
        return NULL;
    }

    tcp->fd = fd;
    xylem_logi("tcp dial fd=%d to %s:%s", (int)fd,
               dial->host, dial->port_str);

    if (tcp->opts.disable_mss_clamp) {
        platform_socket_enable_mss_clamp(fd, false);
    }

    tcp->io = xylem_loop_create_io(loop, tcp->fd);
    if (!tcp->io) {
        _tcp_dial_cleanup(tcp, dial);
        return NULL;
    }

    if (tcp->opts.connect_timeout_ms > 0) {
        dial->connect_timer = xylem_loop_create_timer(loop);
    }
    if (tcp->opts.reconnect_max > 0) {
        dial->reconnect_timer = xylem_loop_create_timer(loop);
    }

    if (connected) {
        if (_tcp_conn_setup(tcp) != 0) {
            xylem_loge("tcp conn fd=%d setup failed", (int)tcp->fd);
            _tcp_dial_cleanup(tcp, dial);
            return NULL;
        }
        xylem_logi("tcp conn fd=%d connected immediately", (int)fd);
        xylem_loop_post(loop, _tcp_post_connect_cb, tcp);
    } else {
        xylem_loop_start_io(tcp->io, XYLEM_POLLER_WR_OP,
                            _tcp_try_connect, tcp);

        if (tcp->opts.connect_timeout_ms > 0 && dial->connect_timer) {
            xylem_loop_start_timer(dial->connect_timer,
                                   _tcp_connect_timeout_cb,
                                   tcp, tcp->opts.connect_timeout_ms, 0);
        }
    }

    return tcp;
}

xylem_tcp_server_t* xylem_tcp_listen(xylem_loop_t* loop,
                                     xylem_addr_t* addr,
                                     xylem_tcp_handler_t* handler,
                                     xylem_tcp_opts_t* opts) {
    xylem_tcp_server_t* server =
        (xylem_tcp_server_t*)calloc(1, sizeof(xylem_tcp_server_t));
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

    if (server->opts.disable_mss_clamp) {
        platform_socket_enable_mss_clamp(fd, false);
    }

    server->io = xylem_loop_create_io(loop, server->fd);
    if (!server->io) {
        platform_socket_close(fd);
        free(server);
        return NULL;
    }
    xylem_loop_start_io(server->io, XYLEM_POLLER_RD_OP,
                        _tcp_server_io_cb, server);

    xylem_logi("tcp server fd=%d listening on %s:%s",
               (int)fd, host, port_str);
    return server;
}

void xylem_tcp_conn_acquire(xylem_tcp_conn_t* tcp) {
    atomic_fetch_add(&tcp->refcount, 1);
}

void xylem_tcp_conn_release(xylem_tcp_conn_t* tcp) {
    _tcp_conn_decref(tcp);
}

void* xylem_tcp_server_get_userdata(xylem_tcp_server_t* server) {
    return server->userdata;
}

void xylem_tcp_server_set_userdata(xylem_tcp_server_t* server, void* ud) {
    server->userdata = ud;
}
