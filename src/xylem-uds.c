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

#include "xylem/xylem-uds.h"
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

#define UDS_DEFAULT_READ_BUF_SIZE 65536
/**
 * sun_path is 104 bytes on macOS, 108 on Linux/Windows.
 * Use the smallest limit for cross-platform safety.
 */
#define UDS_MAX_PATH              104

typedef enum {
    UDS_STATE_CONNECTING,
    UDS_STATE_CONNECTED,
    UDS_STATE_CLOSING,
    UDS_STATE_CLOSED,
} _uds_state_t;

typedef struct _uds_write_req_s {
    xylem_queue_node_t node;
    size_t             len;
    size_t             offset;
    char               data[];
} _uds_write_req_t;

typedef struct _uds_deferred_send_s {
    xylem_uds_conn_t* conn;
    size_t            len;
    char              data[];
} _uds_deferred_send_t;

struct xylem_uds_conn_s {
    xylem_loop_t*         loop;
    xylem_loop_io_t*      io;
    platform_sock_t       fd;
    xylem_uds_handler_t*  handler;
    xylem_uds_opts_t      opts;
    _Atomic _uds_state_t  state;
    _Atomic int32_t       refcount;
    uint8_t*              read_buf;
    size_t                read_len;
    size_t                read_cap;
    xylem_queue_t         write_queue;
    xylem_loop_timer_t*   read_timer;
    xylem_loop_timer_t*   write_timer;
    xylem_loop_timer_t*   heartbeat_timer;
    xylem_list_node_t     server_node;
    xylem_uds_server_t*   server;
    void*                 userdata;
};

struct xylem_uds_server_s {
    xylem_loop_t*         loop;
    xylem_loop_io_t*      io;
    platform_sock_t       fd;
    xylem_uds_handler_t*  handler;
    xylem_uds_opts_t      opts;
    xylem_list_t          connections;
    char                  path[UDS_MAX_PATH];
    void*                 userdata;
    bool                  closing;
};

/**
 * Extract one complete frame from the connection's read buffer.
 * Returns > 0 on success (bytes consumed), 0 if data insufficient,
 * < 0 on error.
 */
static ssize_t _uds_extract_frame(xylem_uds_conn_t* conn,
                                  void** frame_out,
                                  size_t* frame_len_out) {
    uint8_t* data  = conn->read_buf;
    size_t   avail = conn->read_len;

    if (avail == 0) {
        return 0;
    }

    switch (conn->opts.framing.type) {

    case XYLEM_UDS_FRAME_NONE: {
        *frame_out     = data;
        *frame_len_out = avail;
        return (ssize_t)avail;
    }

    case XYLEM_UDS_FRAME_FIXED: {
        size_t fsz = conn->opts.framing.fixed.frame_size;
        if (fsz == 0) {
            xylem_loge("uds conn fd=%d frame_fixed: frame_size=0",
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

    case XYLEM_UDS_FRAME_LENGTH: {
        uint32_t hdr_sz  = conn->opts.framing.length.header_size;
        uint32_t len_off = conn->opts.framing.length.field_offset;
        uint32_t len_sz  = conn->opts.framing.length.field_size;
        int32_t  adj     = conn->opts.framing.length.adjustment;

        if (avail < hdr_sz) {
            return 0;
        }

        uint32_t effective_hdr = hdr_sz;
        uint64_t payload_len   = 0;

        if (conn->opts.framing.length.coding == XYLEM_UDS_LENGTH_FIXEDINT) {
            if (len_sz == 0 || len_sz > 8) {
                xylem_loge("uds conn fd=%d frame_length: invalid "
                           "field_size=%u", (int)conn->fd, len_sz);
                return -1;
            }
            if (len_off > avail || len_sz > avail - len_off) {
                return 0;
            }
            if (conn->opts.framing.length.field_big_endian) {
                for (uint32_t i = 0; i < len_sz; i++) {
                    payload_len = (payload_len << 8) | data[len_off + i];
                }
            } else {
                for (uint32_t i = 0; i < len_sz; i++) {
                    payload_len |=
                        (uint64_t)data[len_off + i] << (i * 8);
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
                xylem_loge("uds conn fd=%d frame_length: varint underflow",
                           (int)conn->fd);
                return -1;
            }
            effective_hdr = hdr_sz + varint_bytes - len_sz;
        }

        if (payload_len >= (uint64_t)INT64_MAX) {
            xylem_loge("uds conn fd=%d frame_length: payload_len overflow",
                       (int)conn->fd);
            return -1;
        }

        int64_t frame_size = (int64_t)effective_hdr +
                             (int64_t)payload_len + (int64_t)adj;
        if (frame_size <= 0) {
            xylem_loge("uds conn fd=%d frame_length: frame_size=%"
                       PRId64 " <= 0", (int)conn->fd, frame_size);
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

    case XYLEM_UDS_FRAME_DELIM: {
        const char* delim     = conn->opts.framing.delim.delim;
        size_t      delim_len = conn->opts.framing.delim.delim_len;
        if (!delim || delim_len == 0) {
            xylem_loge("uds conn fd=%d frame_delim: delim is NULL or "
                       "empty", (int)conn->fd);
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

    case XYLEM_UDS_FRAME_CUSTOM: {
        if (!conn->opts.framing.custom.parse) {
            xylem_loge("uds conn fd=%d frame_custom: parse is NULL",
                       (int)conn->fd);
            return -1;
        }

        int rc = conn->opts.framing.custom.parse(data, avail);

        if (rc > 0) {
            if ((size_t)rc > avail) {
                xylem_loge("uds conn fd=%d frame_custom: parse returned "
                           "%d > avail %zu", (int)conn->fd, rc, avail);
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

static void _uds_read_timeout_cb(xylem_loop_t* loop,
                                 xylem_loop_timer_t* timer,
                                 void* ud) {
    (void)loop;
    (void)timer;
    xylem_uds_conn_t* conn = (xylem_uds_conn_t*)ud;

    xylem_logw("uds conn fd=%d read timeout", (int)conn->fd);
    if (conn->handler && conn->handler->on_timeout) {
        conn->handler->on_timeout(conn, XYLEM_UDS_TIMEOUT_READ);
    }
}

static void _uds_write_timeout_cb(xylem_loop_t* loop,
                                  xylem_loop_timer_t* timer,
                                  void* ud) {
    (void)loop;
    (void)timer;
    xylem_uds_conn_t* conn = (xylem_uds_conn_t*)ud;

    xylem_logw("uds conn fd=%d write timeout", (int)conn->fd);
    if (conn->handler && conn->handler->on_timeout) {
        conn->handler->on_timeout(conn, XYLEM_UDS_TIMEOUT_WRITE);
    }
}

static void _uds_heartbeat_timeout_cb(xylem_loop_t* loop,
                                      xylem_loop_timer_t* timer,
                                      void* ud) {
    (void)loop;
    (void)timer;
    xylem_uds_conn_t* conn = (xylem_uds_conn_t*)ud;

    xylem_logw("uds conn fd=%d heartbeat miss", (int)conn->fd);
    if (conn->handler && conn->handler->on_heartbeat_miss) {
        conn->handler->on_heartbeat_miss(conn);
    }
}

/* Decrement refcount; free the connection when it reaches zero. */
static void _uds_conn_decref(xylem_uds_conn_t* conn) {
    if (atomic_fetch_sub(&conn->refcount, 1) == 1) {
        free(conn);
    }
}

/* Post callback: free a connection after the current iteration. */
static void _uds_conn_free_cb(xylem_loop_t* loop,
                               xylem_loop_post_t* req,
                               void* ud) {
    (void)loop;
    (void)req;
    _uds_conn_decref((xylem_uds_conn_t*)ud);
}

static void _uds_destroy_conn(xylem_uds_conn_t* conn, int err) {
    atomic_store(&conn->state, UDS_STATE_CLOSED);
    xylem_logd("uds conn fd=%d destroy err=%d (%s)", (int)conn->fd, err,
               err ? platform_socket_tostring(err) : "ok");

    if (conn->server) {
        xylem_list_remove(&conn->server->connections, &conn->server_node);
        conn->server = NULL;
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

    if (conn->handler && conn->handler->on_close) {
        const char* errmsg;
        if (err == 0) {
            errmsg = "closed normally";
        } else if (err < 0) {
            errmsg = "internal error";
        } else {
            errmsg = platform_socket_tostring(err);
        }
        conn->handler->on_close(conn, err, errmsg);
    }

    xylem_loop_post(conn->loop, _uds_conn_free_cb, conn);
}

static void _uds_close_conn(xylem_uds_conn_t* conn, int err) {
    if (atomic_load(&conn->state) == UDS_STATE_CLOSED ||
        atomic_load(&conn->state) == UDS_STATE_CLOSING) {
        return;
    }

    xylem_logd("uds conn fd=%d start_close err=%d (%s)", (int)conn->fd, err,
               err ? platform_socket_tostring(err) : "ok");
    atomic_store(&conn->state, UDS_STATE_CLOSING);

    while (!xylem_queue_empty(&conn->write_queue)) {
        xylem_queue_node_t* node =
            xylem_queue_dequeue(&conn->write_queue);
        _uds_write_req_t* req =
            xylem_queue_entry(node, _uds_write_req_t, node);

        if (conn->handler && conn->handler->on_write_done) {
            conn->handler->on_write_done(conn, req->data, req->len, -1);
        }

        free(req);
    }

    _uds_destroy_conn(conn, err);
}

static void _uds_conn_io_cb(xylem_loop_t* loop,
                             xylem_loop_io_t* io,
                             xylem_poller_op_t revents,
                             void* ud);

static void _uds_conn_readable_cb(xylem_uds_conn_t* conn) {
    for (;;) {
        size_t space = conn->read_cap - conn->read_len;
        if (space == 0) {
            xylem_logw("uds conn fd=%d read buffer full, closing",
                       (int)conn->fd);
            _uds_close_conn(conn, -1);
            return;
        }

        ssize_t nread = platform_socket_recv(
            conn->fd, conn->read_buf + conn->read_len, (int)space);

        if (nread == 0) {
            xylem_logi("uds conn fd=%d peer closed", (int)conn->fd);
            _uds_close_conn(conn, 0);
            return;
        }

        if (nread < 0) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                break;
            }
            xylem_logw("uds conn fd=%d recv error=%d (%s)",
                       (int)conn->fd, err,
                       platform_socket_tostring(err));
            _uds_close_conn(conn, err);
            return;
        }

        conn->read_len += (size_t)nread;

        for (;;) {
            void*  frame_data = NULL;
            size_t frame_len  = 0;
            ssize_t rc = _uds_extract_frame(conn, &frame_data, &frame_len);

            if (rc > 0) {
                if (conn->handler && conn->handler->on_read) {
                    conn->handler->on_read(conn, frame_data, frame_len);
                }

                /**
                 * Revalidate after on_read: user may have called
                 * xylem_uds_close inside the callback, which frees
                 * read_buf. Compacting or continuing the recv/extract
                 * loop would touch freed memory.
                 */
                if (atomic_load(&conn->state) == UDS_STATE_CLOSED ||
                    atomic_load(&conn->state) == UDS_STATE_CLOSING) {
                    return;
                }

                conn->read_len -= (size_t)rc;
                if (conn->read_len > 0) {
                    memmove(conn->read_buf,
                            conn->read_buf + (size_t)rc,
                            conn->read_len);
                }
            } else if (rc == 0) {
                break;
            } else {
                _uds_close_conn(conn, -1);
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

static void _uds_flush_writes(xylem_uds_conn_t* conn) {
    while (!xylem_queue_empty(&conn->write_queue)) {
        xylem_queue_node_t* front =
            xylem_queue_front(&conn->write_queue);
        _uds_write_req_t* req =
            xylem_queue_entry(front, _uds_write_req_t, node);

        char*  ptr = req->data + req->offset;
        size_t rem = req->len - req->offset;

        ssize_t n = platform_socket_send(conn->fd, ptr, (int)rem);

        if (n < 0) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                return;
            }

            xylem_logw("uds conn fd=%d send error=%d (%s)",
                       (int)conn->fd, err,
                       platform_socket_tostring(err));

            if (atomic_load(&conn->state) == UDS_STATE_CLOSING) {
                while (!xylem_queue_empty(&conn->write_queue)) {
                    xylem_queue_node_t* qn =
                        xylem_queue_dequeue(&conn->write_queue);
                    _uds_write_req_t* wr =
                        xylem_queue_entry(qn, _uds_write_req_t, node);
                    if (conn->handler && conn->handler->on_write_done) {
                        conn->handler->on_write_done(
                            conn, wr->data, wr->len, -1);
                    }
                    free(wr);
                }
                _uds_destroy_conn(conn, err);
            } else {
                _uds_close_conn(conn, err);
            }
            return;
        }

        req->offset += (size_t)n;

        if (req->offset == req->len) {
            xylem_queue_dequeue(&conn->write_queue);

            if (conn->handler && conn->handler->on_write_done) {
                conn->handler->on_write_done(
                    conn, req->data, req->len, 0);
            }

            free(req);

            if (atomic_load(&conn->state) == UDS_STATE_CLOSED ||
                atomic_load(&conn->state) == UDS_STATE_CLOSING) {
                return;
            }

            if (conn->opts.write_timeout_ms > 0 && conn->write_timer &&
                !xylem_queue_empty(&conn->write_queue)) {
                xylem_loop_reset_timer(conn->write_timer,
                                       conn->opts.write_timeout_ms);
            }
        } else {
            return;
        }
    }

    if (conn->opts.write_timeout_ms > 0 && conn->write_timer) {
        xylem_loop_stop_timer(conn->write_timer);
    }

    if (atomic_load(&conn->state) == UDS_STATE_CLOSING) {
        shutdown(conn->fd, PLATFORM_SHUT_WR);
        _uds_destroy_conn(conn, 0);
    }
}

static void _uds_conn_io_cb(xylem_loop_t* loop,
                             xylem_loop_io_t* io,
                             xylem_poller_op_t revents,
                             void* ud) {
    (void)loop;
    (void)io;
    xylem_uds_conn_t* conn = (xylem_uds_conn_t*)ud;

    if (revents & XYLEM_POLLER_RD_OP) {
        _uds_conn_readable_cb(conn);
    }

    if (atomic_load(&conn->state) == UDS_STATE_CLOSED) {
        return;
    }

    if (revents & XYLEM_POLLER_WR_OP) {
        _uds_flush_writes(conn);

        if (atomic_load(&conn->state) == UDS_STATE_CONNECTED &&
            xylem_queue_empty(&conn->write_queue)) {
            xylem_loop_start_io(conn->io, XYLEM_POLLER_RD_OP,
                                _uds_conn_io_cb, conn);
        }
    }
}

/**
 * Common setup for a connected UDS socket: allocate read buffer,
 * start IO, start heartbeat/read timers.
 */
static int _uds_setup_conn(xylem_uds_conn_t* conn) {
    atomic_store(&conn->state, UDS_STATE_CONNECTED);
    conn->read_buf = (uint8_t*)malloc(conn->opts.read_buf_size);
    if (!conn->read_buf) {
        return -1;
    }
    conn->read_len = 0;
    conn->read_cap = conn->opts.read_buf_size;

    if (xylem_loop_start_io(conn->io, XYLEM_POLLER_RD_OP,
                            _uds_conn_io_cb, conn) != 0) {
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
                                   _uds_heartbeat_timeout_cb,
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
                                   _uds_read_timeout_cb,
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

/* Dial IO callback: check SO_ERROR after non-blocking connect. */
static void _uds_try_connect(xylem_loop_t* loop,
                              xylem_loop_io_t* io,
                              xylem_poller_op_t revents,
                              void* ud) {
    (void)loop;
    (void)io;
    (void)revents;
    xylem_uds_conn_t* conn = (xylem_uds_conn_t*)ud;

    int       err    = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen) != 0) {
        err = platform_socket_get_lasterror();
        if (err == 0) {
            err = -1;
        }
    }

    if (err != 0) {
        xylem_loge("uds conn fd=%d connect failed err=%d (%s)",
                   (int)conn->fd, err,
                   platform_socket_tostring(err));
        _uds_destroy_conn(conn, err);
        return;
    }

    if (_uds_setup_conn(conn) != 0) {
        _uds_close_conn(conn, -1);
        return;
    }

    xylem_logi("uds conn fd=%d connected", (int)conn->fd);
    if (conn->handler && conn->handler->on_connect) {
        conn->handler->on_connect(conn);
    }
}

static void _uds_server_io_cb(xylem_loop_t* loop,
                               xylem_loop_io_t* io,
                               xylem_poller_op_t revents,
                               void* ud) {
    (void)io;
    (void)revents;
    xylem_uds_server_t* server = (xylem_uds_server_t*)ud;

    for (;;) {
        platform_sock_t client_fd =
            platform_socket_accept(server->fd, true);

        if (client_fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN ||
                err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                break;
            }
            xylem_logw("uds server fd=%d accept error=%d (%s)",
                       (int)server->fd, err,
                       platform_socket_tostring(err));
            continue;
        }

        xylem_uds_conn_t* conn =
            (xylem_uds_conn_t*)calloc(1, sizeof(*conn));
        if (!conn) {
            xylem_logw("uds server fd=%d accept: conn alloc failed",
                       (int)server->fd);
            platform_socket_close(client_fd);
            continue;
        }

        conn->loop    = loop;
        conn->fd      = client_fd;
        conn->handler = server->handler;
        conn->opts    = server->opts;

        atomic_store(&conn->refcount, 1);

        xylem_queue_init(&conn->write_queue);

        conn->io = xylem_loop_create_io(loop, client_fd);
        if (!conn->io) {
            xylem_logw("uds server fd=%d accept: io creation failed for fd=%d",
                       (int)server->fd, (int)client_fd);
            platform_socket_close(client_fd);
            free(conn);
            continue;
        }

        if (_uds_setup_conn(conn) != 0) {
            xylem_logw("uds server fd=%d accept: setup failed for fd=%d",
                       (int)server->fd, (int)client_fd);
            xylem_loop_destroy_io(conn->io);
            platform_socket_close(client_fd);
            free(conn);
            continue;
        }

        conn->server = server;
        xylem_list_insert_tail(&server->connections,
                               &conn->server_node);

        xylem_logi("uds server fd=%d accepted conn fd=%d",
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
static void _uds_server_free_cb(xylem_loop_t* loop,
                                 xylem_loop_post_t* req,
                                 void* ud) {
    (void)loop;
    (void)req;
    free(ud);
}

void xylem_uds_close_server(xylem_uds_server_t* server) {
    if (server->closing) {
        return;
    }

    xylem_logi("uds server fd=%d closing", (int)server->fd);
    server->closing = true;

    xylem_loop_destroy_io(server->io);
    server->io = NULL;
    platform_socket_close(server->fd);

    while (!xylem_list_empty(&server->connections)) {
        xylem_list_node_t* node = xylem_list_head(&server->connections);
        xylem_uds_conn_t* conn =
            xylem_list_entry(node, xylem_uds_conn_t, server_node);
        xylem_list_remove(&server->connections, node);
        conn->server = NULL;
        xylem_uds_close(conn);
    }

    /* Unlink the socket file. */
    if (server->path[0] != '\0') {
        remove(server->path);
    }

    xylem_loop_post(server->loop, _uds_server_free_cb, server);
}

/**
 * Close logic that runs on the loop thread.  Extracted so that
 * _uds_deferred_close_cb can call it directly instead of re-entering
 * xylem_uds_close (which would re-post when loop->tid is unset).
 */
static void _uds_do_close(xylem_uds_conn_t* conn) {
    if (atomic_load(&conn->state) == UDS_STATE_CLOSING ||
        atomic_load(&conn->state) == UDS_STATE_CLOSED) {
        return;
    }

    xylem_logi("uds conn fd=%d graceful close requested",
               (int)conn->fd);
    atomic_store(&conn->state, UDS_STATE_CLOSING);

    if (xylem_queue_empty(&conn->write_queue)) {
        shutdown(conn->fd, PLATFORM_SHUT_WR);
        _uds_destroy_conn(conn, 0);
    }
}

static void _uds_deferred_close_cb(xylem_loop_t* loop,
                                    xylem_loop_post_t* req,
                                    void* ud) {
    (void)loop;
    (void)req;
    xylem_uds_conn_t* conn = (xylem_uds_conn_t*)ud;
    _uds_do_close(conn);
    _uds_conn_decref(conn);
}

void xylem_uds_close(xylem_uds_conn_t* conn) {
    /* Idempotent: reject if already closing/closed. */
    _uds_state_t st = atomic_load(&conn->state);
    if (st == UDS_STATE_CLOSING || st == UDS_STATE_CLOSED) {
        return;
    }

    /* Cross-thread: post to loop thread. */
    if (!xylem_loop_is_loop_thread(conn->loop)) {
        atomic_fetch_add(&conn->refcount, 1);
        if (xylem_loop_post(conn->loop, _uds_deferred_close_cb, conn) != 0) {
            _uds_conn_decref(conn);
        }
        return;
    }

    _uds_do_close(conn);
}

static int _uds_enqueue_write(xylem_uds_conn_t* conn,
                              const void* data, size_t len) {
    _uds_write_req_t* req =
        (_uds_write_req_t*)malloc(sizeof(*req) + len);
    if (!req) {
        return -1;
    }

    req->len    = len;
    req->offset = 0;
    memcpy(req->data, data, len);

    bool was_empty = xylem_queue_empty(&conn->write_queue);
    xylem_queue_enqueue(&conn->write_queue, &req->node);

    if (was_empty) {
        xylem_loop_start_io(conn->io,
                            XYLEM_POLLER_RD_OP | XYLEM_POLLER_WR_OP,
                            _uds_conn_io_cb, conn);

        if (conn->opts.write_timeout_ms > 0 && conn->write_timer) {
            xylem_loop_start_timer(conn->write_timer,
                                   _uds_write_timeout_cb,
                                   conn, conn->opts.write_timeout_ms, 0);
        }
    }

    return 0;
}

static void _uds_deferred_send_cb(xylem_loop_t* loop,
                                   xylem_loop_post_t* req,
                                   void* ud) {
    (void)loop;
    (void)req;
    _uds_deferred_send_t* ds = (_uds_deferred_send_t*)ud;

    if (atomic_load(&ds->conn->state) == UDS_STATE_CONNECTED) {
        _uds_enqueue_write(ds->conn, ds->data, ds->len);
    }

    _uds_conn_decref(ds->conn);
    free(ds);
}

int xylem_uds_send(xylem_uds_conn_t* conn,
                   const void* data, size_t len) {
    if (atomic_load(&conn->state) != UDS_STATE_CONNECTED) {
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    /* Cross-thread: copy data and post to loop thread. */
    if (!xylem_loop_is_loop_thread(conn->loop)) {
        _uds_deferred_send_t* ds = (_uds_deferred_send_t*)malloc(
            sizeof(_uds_deferred_send_t) + len);
        if (!ds) {
            return -1;
        }
        ds->conn = conn;
        ds->len  = len;
        memcpy(ds->data, data, len);

        atomic_fetch_add(&conn->refcount, 1);
        if (xylem_loop_post(conn->loop, _uds_deferred_send_cb, ds) != 0) {
            _uds_conn_decref(conn);
            free(ds);
            return -1;
        }
        return 0;
    }

    /* Same thread: enqueue directly. */
    return _uds_enqueue_write(conn, data, len);
}

/**
 * When non-blocking connect succeeds immediately inside xylem_uds_dial,
 * on_connect would fire before dial returns -- the caller has no chance to
 * call set_userdata yet, so the callback sees NULL userdata.  Deferring to
 * the next loop iteration guarantees dial returns first.
 */
static void _uds_deferred_connect_cb(xylem_loop_t* loop,
                                      xylem_loop_post_t* req,
                                      void* ud) {
    (void)loop;
    (void)req;
    xylem_uds_conn_t* conn = (xylem_uds_conn_t*)ud;
    if (atomic_load(&conn->state) == UDS_STATE_CLOSED ||
        atomic_load(&conn->state) == UDS_STATE_CLOSING) {
        return;
    }
    if (conn->handler && conn->handler->on_connect) {
        conn->handler->on_connect(conn);
    }
}

/**
 * Roll back a partially initialised dial connection.
 * Each field is NULL-safe: calloc zeroes everything, so only
 * resources that were actually created get released.
 */
static void _uds_dial_cleanup(xylem_uds_conn_t* conn) {
    if (conn->io) {
        xylem_loop_destroy_io(conn->io);
    }
    if (conn->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(conn->fd);
    }
    free(conn);
}

xylem_uds_conn_t* xylem_uds_dial(xylem_loop_t* loop,
                                  const char* path,
                                  xylem_uds_handler_t* handler,
                                  xylem_uds_opts_t* opts) {
    if (!path || strlen(path) >= UDS_MAX_PATH) {
        xylem_loge("uds dial: path is NULL or too long (max %d)",
                   UDS_MAX_PATH - 1);
        return NULL;
    }

    xylem_uds_conn_t* conn =
        (xylem_uds_conn_t*)calloc(1, sizeof(*conn));
    if (!conn) {
        return NULL;
    }

    if (opts) {
        conn->opts = *opts;
    }
    if (conn->opts.read_buf_size == 0) {
        conn->opts.read_buf_size = UDS_DEFAULT_READ_BUF_SIZE;
    }

    conn->loop    = loop;
    conn->handler = handler;
    atomic_store(&conn->state, UDS_STATE_CONNECTING);

    atomic_store(&conn->refcount, 1);

    xylem_queue_init(&conn->write_queue);

    bool connected = false;
    platform_sock_t fd = platform_socket_dial_unix(path, &connected, true);

    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("uds dial: socket creation failed for %s",
                   path ? path : "(null)");
        free(conn);
        return NULL;
    }

    conn->fd = fd;
    xylem_logi("uds dial fd=%d to %s", (int)fd, path ? path : "(null)");

    conn->io = xylem_loop_create_io(loop, conn->fd);
    if (!conn->io) {
        xylem_loge("uds dial fd=%d: io creation failed", (int)fd);
        _uds_dial_cleanup(conn);
        return NULL;
    }

    if (connected) {
        if (_uds_setup_conn(conn) != 0) {
            _uds_dial_cleanup(conn);
            return NULL;
        }
        xylem_logi("uds conn fd=%d connected immediately", (int)fd);
        xylem_loop_post(loop, _uds_deferred_connect_cb, conn);
    } else {
        xylem_loop_start_io(conn->io, XYLEM_POLLER_WR_OP,
                            _uds_try_connect, conn);
    }

    return conn;
}

/**
 * Roll back a partially initialised listen server.
 * Each field is NULL-safe: calloc zeroes everything, so only
 * resources that were actually created get released.
 */
static void _uds_listen_cleanup(xylem_uds_server_t* server) {
    if (server->io) {
        xylem_loop_destroy_io(server->io);
    }
    if (server->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(server->fd);
    }
    free(server);
}

xylem_uds_server_t* xylem_uds_listen(xylem_loop_t* loop,
                                      const char* path,
                                      xylem_uds_handler_t* handler,
                                      xylem_uds_opts_t* opts) {
    xylem_uds_server_t* server =
        (xylem_uds_server_t*)calloc(1, sizeof(*server));
    if (!server) {
        return NULL;
    }

    if (opts) {
        server->opts = *opts;
    }
    if (server->opts.read_buf_size == 0) {
        server->opts.read_buf_size = UDS_DEFAULT_READ_BUF_SIZE;
    }

    server->loop    = loop;
    server->handler = handler;
    server->closing = false;

    xylem_list_init(&server->connections);

    if (!path || strlen(path) >= UDS_MAX_PATH) {
        free(server);
        xylem_loge("uds listen: path is NULL or too long (max %d)",
                   UDS_MAX_PATH - 1);
        return NULL;
    }
    snprintf(server->path, UDS_MAX_PATH, "%s", path);

    platform_sock_t fd = platform_socket_listen_unix(path, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        free(server);
        xylem_loge("uds listen: socket creation failed for %s", path);
        return NULL;
    }

    server->fd = fd;

    server->io = xylem_loop_create_io(loop, server->fd);
    if (!server->io) {
        xylem_loge("uds listen fd=%d: io creation failed", (int)fd);
        _uds_listen_cleanup(server);
        return NULL;
    }
    xylem_loop_start_io(server->io, XYLEM_POLLER_RD_OP,
                        _uds_server_io_cb, server);

    xylem_logi("uds server fd=%d listening on %s",
               (int)fd, path ? path : "(null)");
    return server;
}

xylem_loop_t* xylem_uds_get_loop(xylem_uds_conn_t* conn) {
    return conn->loop;
}

void* xylem_uds_get_userdata(xylem_uds_conn_t* conn) {
    return conn->userdata;
}

void xylem_uds_set_userdata(xylem_uds_conn_t* conn, void* ud) {
    conn->userdata = ud;
}

void xylem_uds_conn_acquire(xylem_uds_conn_t* conn) {
    atomic_fetch_add(&conn->refcount, 1);
}

void xylem_uds_conn_release(xylem_uds_conn_t* conn) {
    _uds_conn_decref(conn);
}

void* xylem_uds_server_get_userdata(xylem_uds_server_t* server) {
    return server->userdata;
}

void xylem_uds_server_set_userdata(xylem_uds_server_t* server, void* ud) {
    server->userdata = ud;
}
