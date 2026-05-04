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

#include "xylem/net/xylem-tcp.h"
#include "xylem/xylem-logger.h"

#include "runtime/runtime.h"
#include "addr.h"
#include "platform/platform-socket.h"

#include "minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_READ_BUF_SIZE 65536

enum {
    TCP_IO_IDLE    = 0,
    TCP_IO_WAITING = 1,
    TCP_IO_READY   = 2,
};

struct xylem_tcp_conn_s {
    loop_t*              loop;
    loop_io_t*           io;
    loop_timer_t*        rd_timer;
    loop_timer_t*        wr_timer;
    platform_sock_t      fd;
    mco_coro*            read_coro;
    mco_coro*            write_coro;
    _Atomic int          rd_state;
    _Atomic int          wr_state;
    addr_t               peer_addr;
    void*                userdata;
    uint64_t             read_timeout_ms;
    uint64_t             write_timeout_ms;
    xylem_tcp_frame_opts_t frame_opts;
    char*                read_buf;
    size_t               read_buf_cap;
    size_t               read_buf_pos;
    size_t               read_buf_len;
    int                  last_error;
    bool                 rd_timed_out;
    bool                 wr_timed_out;
    bool                 closed;
};

struct xylem_tcp_listener_s {
    loop_t*         loop;
    loop_io_t*      io;
    platform_sock_t fd;
    mco_coro*       wait_coro;
    void*           userdata;
    uint64_t        read_timeout_ms;
    uint64_t        write_timeout_ms;
    size_t          max_read_buf;
    bool            closing;
};

/**
 * Try to transition a coro from WAITING to READY and schedule it.
 * If still IDLE (coro hasn't yielded yet), mark READY so the coro
 * skips the yield when it gets there.
 * Returns the coro pointer if it was scheduled, NULL otherwise.
 */
static mco_coro* _tcp_io_wake(_Atomic int* state, mco_coro** coro_slot) {
    int expected = TCP_IO_WAITING;
    if (atomic_compare_exchange_strong(state, &expected, TCP_IO_READY)) {
        mco_coro* co = *coro_slot;
        *coro_slot = NULL;
        return co;
    }
    /* Still IDLE -- coro hasn't yielded yet. Mark READY so it won't yield. */
    expected = TCP_IO_IDLE;
    atomic_compare_exchange_strong(state, &expected, TCP_IO_READY);
    return NULL;
}

static void _tcp_conn_io_cb(
    loop_t* loop,
    loop_io_t* io,
    loop_poller_op_t revents,
    void* ud) {
    (void)loop;
    (void)io;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;
    scheduler_t* sched = runtime_get_scheduler();

    if ((revents & LOOP_POLLER_RD_OP)) {
        mco_coro* co = _tcp_io_wake(&tcp->rd_state, &tcp->read_coro);
        if (co) {
            scheduler_schedule(sched, co);
        }
    }
    if ((revents & LOOP_POLLER_WR_OP)) {
        mco_coro* co = _tcp_io_wake(&tcp->wr_state, &tcp->write_coro);
        if (co) {
            scheduler_schedule(sched, co);
        }
    }

    loop_poller_op_t remaining = LOOP_POLLER_NO_OP;
    if (tcp->read_coro) {
        remaining |= LOOP_POLLER_RD_OP;
    }
    if (tcp->write_coro) {
        remaining |= LOOP_POLLER_WR_OP;
    }
    if (remaining != LOOP_POLLER_NO_OP) {
        loop_start_io(tcp->io, remaining, _tcp_conn_io_cb, tcp);
    }
}

static void _tcp_rd_timeout_cb(
    loop_t* loop,
    loop_timer_t* timer,
    void* ud) {
    (void)loop;
    (void)timer;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;
    tcp->rd_timed_out = true;
    mco_coro* co = _tcp_io_wake(&tcp->rd_state, &tcp->read_coro);
    if (co) {
        scheduler_schedule(runtime_get_scheduler(), co);
    }
}

static void _tcp_wr_timeout_cb(
    loop_t* loop,
    loop_timer_t* timer,
    void* ud) {
    (void)loop;
    (void)timer;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;
    tcp->wr_timed_out = true;
    mco_coro* co = _tcp_io_wake(&tcp->wr_state, &tcp->write_coro);
    if (co) {
        scheduler_schedule(runtime_get_scheduler(), co);
    }
}

typedef struct {
    xylem_tcp_conn_t* tcp;
    uint64_t          timeout_ms;
    bool              is_read;
} _tcp_timeout_ctx_t;


static void _tcp_timeout_start_cb(
    loop_t* loop, loop_post_t* req, void* ud) {
    (void)req;
    _tcp_timeout_ctx_t* ctx = (_tcp_timeout_ctx_t*)ud;
    xylem_tcp_conn_t* tcp = ctx->tcp;

    if (tcp->closed) {
        free(ctx);
        return;
    }

    if (ctx->is_read) {
        if (atomic_load(&tcp->rd_state) == TCP_IO_IDLE) {
            free(ctx);
            return;
        }
        if (!tcp->rd_timer) {
            tcp->rd_timer = loop_create_timer(loop);
        }
        loop_start_timer(
            tcp->rd_timer, _tcp_rd_timeout_cb, tcp, ctx->timeout_ms, 0);
    } else {
        if (atomic_load(&tcp->wr_state) == TCP_IO_IDLE) {
            free(ctx);
            return;
        }
        if (!tcp->wr_timer) {
            tcp->wr_timer = loop_create_timer(loop);
        }
        loop_start_timer(
            tcp->wr_timer, _tcp_wr_timeout_cb, tcp, ctx->timeout_ms, 0);
    }
    free(ctx);
}

static void _tcp_rd_timeout_stop_cb(
    loop_t* loop, loop_post_t* req, void* ud) {
    (void)loop; (void)req;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;
    if (tcp->rd_timer) {
        loop_stop_timer(tcp->rd_timer);
    }
}

static void _tcp_wr_timeout_stop_cb(
    loop_t* loop, loop_post_t* req, void* ud) {
    (void)loop; (void)req;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;
    if (tcp->wr_timer) {
        loop_stop_timer(tcp->wr_timer);
    }
}

static void _tcp_arm_io_cb(
    loop_t* loop, loop_post_t* req, void* ud) {
    (void)loop; (void)req;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;
    if (tcp->closed) {
        return;
    }

    loop_poller_op_t interest = LOOP_POLLER_NO_OP;
    if (tcp->read_coro) {
        interest |= LOOP_POLLER_RD_OP;
    }
    if (tcp->write_coro) {
        interest |= LOOP_POLLER_WR_OP;
    }
    if (interest != LOOP_POLLER_NO_OP) {
        loop_start_io(tcp->io, interest, _tcp_conn_io_cb, tcp);
    }
}

static bool _tcp_wait_read(xylem_tcp_conn_t* tcp, uint64_t timeout_ms) {
    tcp->rd_timed_out = false;
    tcp->read_coro = mco_running();
    atomic_store(&tcp->rd_state, TCP_IO_IDLE);

    loop_post(tcp->loop, _tcp_arm_io_cb, tcp);

    if (timeout_ms > 0) {
        _tcp_timeout_ctx_t* ctx =
            (_tcp_timeout_ctx_t*)malloc(sizeof(_tcp_timeout_ctx_t));
        if (ctx) {
            ctx->tcp = tcp;
            ctx->timeout_ms = timeout_ms;
            ctx->is_read = true;
            loop_post(tcp->loop, _tcp_timeout_start_cb, ctx);
        }
    }

    /* CAS IDLE -> WAITING. If it fails, IO already set READY before
     * we could yield -- skip the yield entirely. */
    int expected = TCP_IO_IDLE;
    if (atomic_compare_exchange_strong(&tcp->rd_state, &expected,
                                       TCP_IO_WAITING)) {
        mco_yield(mco_running());
    }

    atomic_store(&tcp->rd_state, TCP_IO_IDLE);
    tcp->read_coro = NULL;

    if (timeout_ms > 0) {
        loop_post(tcp->loop, _tcp_rd_timeout_stop_cb, tcp);
    }

    return !tcp->rd_timed_out;
}

static bool _tcp_wait_write(xylem_tcp_conn_t* tcp, uint64_t timeout_ms) {
    tcp->wr_timed_out = false;
    tcp->write_coro = mco_running();
    atomic_store(&tcp->wr_state, TCP_IO_IDLE);

    loop_post(tcp->loop, _tcp_arm_io_cb, tcp);

    if (timeout_ms > 0) {
        _tcp_timeout_ctx_t* ctx =
            (_tcp_timeout_ctx_t*)malloc(sizeof(_tcp_timeout_ctx_t));
        if (ctx) {
            ctx->tcp = tcp;
            ctx->timeout_ms = timeout_ms;
            ctx->is_read = false;
            loop_post(tcp->loop, _tcp_timeout_start_cb, ctx);
        }
    }

    int expected = TCP_IO_IDLE;
    if (atomic_compare_exchange_strong(&tcp->wr_state, &expected,
                                       TCP_IO_WAITING)) {
        mco_yield(mco_running());
    }

    atomic_store(&tcp->wr_state, TCP_IO_IDLE);
    tcp->write_coro = NULL;

    if (timeout_ms > 0) {
        loop_post(tcp->loop, _tcp_wr_timeout_stop_cb, tcp);
    }

    return !tcp->wr_timed_out;
}

static xylem_tcp_conn_t* _tcp_conn_alloc(
    loop_t* loop,
    platform_sock_t fd,
    size_t max_read_buf) {
    xylem_tcp_conn_t* tcp =
        (xylem_tcp_conn_t*)calloc(1, sizeof(xylem_tcp_conn_t));
    if (!tcp) {
        return NULL;
    }

    tcp->loop = loop;
    tcp->fd = fd;
    tcp->io = loop_create_io(loop, (loop_poller_fd_t)fd);
    if (!tcp->io) {
        free(tcp);
        return NULL;
    }

    size_t buf_cap = max_read_buf > 0 ? max_read_buf : DEFAULT_READ_BUF_SIZE;
    tcp->read_buf = (char*)malloc(buf_cap);
    if (!tcp->read_buf) {
        loop_destroy_io(tcp->io);
        free(tcp);
        return NULL;
    }
    tcp->read_buf_cap = buf_cap;

    platform_socket_enable_nodelay(fd, true);
    platform_socket_enable_keepalive(fd, true);
    return tcp;
}

static void _tcp_listener_io_cb(
    loop_t* loop,
    loop_io_t* io,
    loop_poller_op_t revents,
    void* ud) {
    (void)loop;
    (void)io;
    (void)revents;
    xylem_tcp_listener_t* listener = (xylem_tcp_listener_t*)ud;
    fprintf(stderr, "  [listener_io_cb] fd=%d wait_coro=%p\n",
            (int)listener->fd, (void*)listener->wait_coro);
    if (listener->wait_coro) {
        mco_coro* co = listener->wait_coro;
        listener->wait_coro = NULL;
        scheduler_schedule(runtime_get_scheduler(), co);
    }
}

/* --- raw socket read (fills internal buffer) --- */

static int64_t _tcp_raw_recv(xylem_tcp_conn_t* tcp, void* buf, size_t len) {
    if (tcp->closed) {
        return -1;
    }

    uint64_t ms = tcp->read_timeout_ms;

    for (;;) {
        ssize_t n = platform_socket_recv(tcp->fd, buf, (int)len);
        if (n > 0) {
            return n;
        }
        if (n == 0) {
            return 0;
        }

        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN &&
            err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            tcp->last_error = err;
            return -1;
        }

        if (!_tcp_wait_read(tcp, ms) || tcp->closed) {
            tcp->last_error = ms > 0
                ? PLATFORM_SO_ERROR_ETIMEDOUT
                : PLATFORM_SO_ERROR_ECONNRESET;
            return -1;
        }
    }
}

static int _tcp_read_exact(xylem_tcp_conn_t* tcp, void* buf, size_t len) {
    char* ptr = (char*)buf;
    size_t rem = len;

    while (rem > 0) {
        /* Drain internal buffer first. */
        size_t avail = tcp->read_buf_len - tcp->read_buf_pos;
        if (avail > 0) {
            size_t copy = avail < rem ? avail : rem;
            memcpy(ptr, tcp->read_buf + tcp->read_buf_pos, copy);
            tcp->read_buf_pos += copy;
            ptr += copy;
            rem -= copy;
            continue;
        }

        /* Buffer empty — read from socket. */
        tcp->read_buf_pos = 0;
        tcp->read_buf_len = 0;

        int64_t n = _tcp_raw_recv(
            tcp, tcp->read_buf, tcp->read_buf_cap);
        if (n <= 0) {
            return -1;
        }
        tcp->read_buf_len = (size_t)n;
    }
    return 0;
}

static int64_t _tcp_buffered_read(
    xylem_tcp_conn_t* tcp, void* buf, size_t len) {
    /* Return buffered data if available. */
    size_t avail = tcp->read_buf_len - tcp->read_buf_pos;
    if (avail > 0) {
        size_t copy = avail < len ? avail : len;
        memcpy(buf, tcp->read_buf + tcp->read_buf_pos, copy);
        tcp->read_buf_pos += copy;
        return (int64_t)copy;
    }

    /* Buffer empty — read directly to user buf if large enough. */
    if (len >= tcp->read_buf_cap) {
        return _tcp_raw_recv(tcp, buf, len);
    }

    /* Fill internal buffer, then copy. */
    tcp->read_buf_pos = 0;
    tcp->read_buf_len = 0;

    int64_t n = _tcp_raw_recv(tcp, tcp->read_buf, tcp->read_buf_cap);
    if (n <= 0) {
        return n;
    }
    tcp->read_buf_len = (size_t)n;

    size_t copy = (size_t)n < len ? (size_t)n : len;
    memcpy(buf, tcp->read_buf, copy);
    tcp->read_buf_pos = copy;
    return (int64_t)copy;
}

/* --- framed recv implementations --- */

static int64_t _tcp_recv_fixed(
    xylem_tcp_conn_t* tcp, void* buf, size_t len) {
    size_t frame_len = tcp->frame_opts.fixed.len;
    if (frame_len > len) {
        tcp->last_error = -1;
        return -1;
    }
    if (_tcp_read_exact(tcp, buf, frame_len) != 0) {
        return -1;
    }
    return (int64_t)frame_len;
}

static int64_t _tcp_recv_length(
    xylem_tcp_conn_t* tcp, void* buf, size_t len) {
    uint8_t hdr[16];
    uint32_t hdr_sz = tcp->frame_opts.length.header_size;

    if (hdr_sz > sizeof(hdr)) {
        tcp->last_error = -1;
        return -1;
    }

    if (_tcp_read_exact(tcp, hdr, hdr_sz) != 0) {
        return -1;
    }

    uint64_t body_len = 0;
    uint8_t* field = hdr + tcp->frame_opts.length.field_offset;

    if (tcp->frame_opts.length.big_endian) {
        for (uint32_t i = 0; i < tcp->frame_opts.length.field_size; i++) {
            body_len = (body_len << 8) | field[i];
        }
    } else {
        for (uint32_t i = 0; i < tcp->frame_opts.length.field_size; i++) {
            body_len |= (uint64_t)field[i] << (i * 8);
        }
    }

    int64_t adjusted = (int64_t)body_len + tcp->frame_opts.length.adjustment;
    if (adjusted <= 0) {
        tcp->last_error = -1;
        return -1;
    }

    size_t payload_len = (size_t)adjusted;
    if (payload_len > len) {
        tcp->last_error = -1;
        return -1;
    }

    if (_tcp_read_exact(tcp, buf, payload_len) != 0) {
        return -1;
    }
    return (int64_t)payload_len;
}

static int64_t _tcp_recv_delimiter(
    xylem_tcp_conn_t* tcp, void* buf, size_t len) {
    const char* delim = tcp->frame_opts.delimiter.delim;
    size_t delim_len = tcp->frame_opts.delimiter.delim_len;
    if (delim_len == 0) {
        delim_len = strlen(delim);
    }

    char* dst = (char*)buf;
    size_t pos = 0;

    while (pos < len) {
        /* Ensure internal buffer has data. */
        size_t avail = tcp->read_buf_len - tcp->read_buf_pos;
        if (avail == 0) {
            tcp->read_buf_pos = 0;
            tcp->read_buf_len = 0;
            int64_t n = _tcp_raw_recv(
                tcp, tcp->read_buf, tcp->read_buf_cap);
            if (n <= 0) {
                return -1;
            }
            tcp->read_buf_len = (size_t)n;
            avail = (size_t)n;
        }

        /* Scan for delimiter byte-by-byte from internal buffer. */
        char* src = tcp->read_buf + tcp->read_buf_pos;
        for (size_t i = 0; i < avail && pos < len; i++) {
            dst[pos++] = src[i];
            tcp->read_buf_pos++;

            if (pos >= delim_len &&
                memcmp(dst + pos - delim_len, delim, delim_len) == 0) {
                pos -= delim_len;
                dst[pos] = '\0';
                return (int64_t)pos;
            }
        }
    }

    /* Buffer full without finding delimiter. */
    tcp->last_error = -1;
    return -1;
}

/* --- raw send (full write) --- */

static int _tcp_raw_send(
    xylem_tcp_conn_t* tcp,
    const void* data,
    size_t len) {
    if (tcp->closed) {
        return -1;
    }

    uint64_t ms = tcp->write_timeout_ms;
    const char* ptr = (const char*)data;
    size_t rem = len;

    while (rem > 0) {
        ssize_t n = platform_socket_send(tcp->fd, ptr, (int)rem);
        if (n > 0) {
            ptr += n;
            rem -= (size_t)n;
            continue;
        }

        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN &&
            err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            tcp->last_error = err;
            return -1;
        }

        if (!_tcp_wait_write(tcp, ms) || tcp->closed) {
            tcp->last_error = ms > 0
                ? PLATFORM_SO_ERROR_ETIMEDOUT
                : PLATFORM_SO_ERROR_ECONNRESET;
            return -1;
        }
    }
    return 0;
}

/* --- public API --- */

xylem_tcp_conn_t* xylem_tcp_dial(
    const char* host,
    uint16_t port,
    uint64_t connect_timeout_ms,
    xylem_tcp_opts_t* opts) {
    loop_t* loop = runtime_get_loop();
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    const char* dial_host = host;
    char resolved_ip[INET6_ADDRSTRLEN];
    addr_t resolved_addr;

    if (addr_pton(host, port, &resolved_addr) != 0) {
        addr_t* addrs = NULL;
        size_t count = 0;
        if (addr_resolve(host, &addrs, &count) != 0 || count == 0) {
            xylem_loge("tcp dial: DNS resolution failed for %s", host);
            return NULL;
        }
        resolved_addr = addrs[0];
        free(addrs);
        uint16_t rport;
        addr_ntop(&resolved_addr, resolved_ip, sizeof(resolved_ip), &rport);
        dial_host = resolved_ip;
    }

    bool connected = false;
    platform_sock_t fd = platform_socket_dial(
        dial_host, port_str, SOCK_STREAM, &connected, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("tcp dial: socket creation failed for %s:%s",
                   host, port_str);
        return NULL;
    }

    if (opts && opts->disable_mss_clamp) {
        platform_socket_enable_mss_clamp(fd, false);
    }

    size_t max_buf = opts ? opts->max_read_buf : 0;
    xylem_tcp_conn_t* tcp = _tcp_conn_alloc(loop, fd, max_buf);
    if (!tcp) {
        platform_socket_close(fd);
        return NULL;
    }

    tcp->peer_addr = resolved_addr;
    if (opts) {
        tcp->read_timeout_ms = opts->read_timeout_ms;
        tcp->write_timeout_ms = opts->write_timeout_ms;
    }

    fprintf(stderr, "  [dial] fd=%d connected=%d\n", (int)fd, connected);
    if (!connected) {
        fprintf(stderr, "  [dial] wait_write...\n");
        if (!_tcp_wait_write(tcp, connect_timeout_ms)) {
            fprintf(stderr, "  [dial] wait_write timeout\n");
            tcp->last_error = PLATFORM_SO_ERROR_ETIMEDOUT;
            xylem_tcp_close(tcp);
            return NULL;
        }
        fprintf(stderr, "  [dial] wait_write done\n");

        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen);
        fprintf(stderr, "  [dial] SO_ERROR=%d\n", err);
        if (err != 0) {
            tcp->last_error = err;
            xylem_loge("tcp dial fd=%d connect error=%d (%s)",
                       (int)fd, err, platform_socket_tostring(err));
            xylem_tcp_close(tcp);
            return NULL;
        }
    }

    xylem_logi("tcp conn fd=%d connected to %s:%s", (int)fd, host, port_str);
    return tcp;
}

void xylem_tcp_set_framing(
    xylem_tcp_conn_t* tcp,
    xylem_tcp_frame_opts_t* opts) {
    if (opts) {
        tcp->frame_opts = *opts;
    } else {
        memset(&tcp->frame_opts, 0, sizeof(tcp->frame_opts));
    }
}

int64_t xylem_tcp_recv(
    xylem_tcp_conn_t* tcp,
    void* buf,
    size_t len) {
    switch (tcp->frame_opts.type) {
    case XYLEM_TCP_FRAME_NONE:
        return _tcp_buffered_read(tcp, buf, len);
    case XYLEM_TCP_FRAME_FIXED:
        return _tcp_recv_fixed(tcp, buf, len);
    case XYLEM_TCP_FRAME_LENGTH:
        return _tcp_recv_length(tcp, buf, len);
    case XYLEM_TCP_FRAME_DELIMITER:
        return _tcp_recv_delimiter(tcp, buf, len);
    }
    return -1;
}

int xylem_tcp_send(
    xylem_tcp_conn_t* tcp,
    const void* data,
    size_t len) {
    if (tcp->frame_opts.type == XYLEM_TCP_FRAME_LENGTH) {
        uint8_t hdr[16];
        uint32_t hdr_sz = tcp->frame_opts.length.header_size;

        if (hdr_sz > sizeof(hdr)) {
            return -1;
        }

        memset(hdr, 0, hdr_sz);

        int64_t wire_len = (int64_t)len - tcp->frame_opts.length.adjustment;
        if (wire_len < 0) {
            return -1;
        }

        uint8_t* field = hdr + tcp->frame_opts.length.field_offset;
        uint64_t val = (uint64_t)wire_len;

        if (tcp->frame_opts.length.big_endian) {
            for (int i = (int)tcp->frame_opts.length.field_size - 1;
                 i >= 0; i--) {
                field[i] = (uint8_t)(val & 0xFF);
                val >>= 8;
            }
        } else {
            for (uint32_t i = 0; i < tcp->frame_opts.length.field_size; i++) {
                field[i] = (uint8_t)(val & 0xFF);
                val >>= 8;
            }
        }

        if (_tcp_raw_send(tcp, hdr, hdr_sz) != 0) {
            return -1;
        }
    }

    return _tcp_raw_send(tcp, data, len);
}

xylem_tcp_listener_t* xylem_tcp_listen(
    const char* host,
    uint16_t port,
    xylem_tcp_opts_t* opts) {
    loop_t* loop = runtime_get_loop();
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd = platform_socket_listen(
        host, port_str, SOCK_STREAM, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("tcp listen: failed for %s:%s", host, port_str);
        return NULL;
    }

    if (opts && opts->disable_mss_clamp) {
        platform_socket_enable_mss_clamp(fd, false);
    }

    xylem_tcp_listener_t* listener =
        (xylem_tcp_listener_t*)calloc(1, sizeof(xylem_tcp_listener_t));
    if (!listener) {
        platform_socket_close(fd);
        return NULL;
    }

    listener->loop = loop;
    listener->fd = fd;
    if (opts) {
        listener->read_timeout_ms = opts->read_timeout_ms;
        listener->write_timeout_ms = opts->write_timeout_ms;
        listener->max_read_buf = opts->max_read_buf;
    }
    listener->io = loop_create_io(loop, (loop_poller_fd_t)fd);
    if (!listener->io) {
        platform_socket_close(fd);
        free(listener);
        return NULL;
    }

    xylem_logi("tcp listener fd=%d listening on %s:%s",
               (int)fd, host, port_str);
    return listener;
}

static void _tcp_listener_arm_io_cb(
    loop_t* loop, loop_post_t* req, void* ud) {
    (void)loop; (void)req;
    xylem_tcp_listener_t* listener = (xylem_tcp_listener_t*)ud;
    if (listener->closing) {
        return;
    }
    loop_start_io(
        listener->io, LOOP_POLLER_RD_OP, _tcp_listener_io_cb, listener);
}

xylem_tcp_conn_t* xylem_tcp_accept(xylem_tcp_listener_t* listener) {
    for (;;) {
        if (listener->closing) {
            return NULL;
        }

        platform_sock_t fd = platform_socket_accept(listener->fd, true);
        if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            int err = platform_socket_get_lasterror();
            if (err != PLATFORM_SO_ERROR_EAGAIN &&
                err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
                xylem_logw("tcp listener fd=%d accept error=%d (%s)",
                           (int)listener->fd, err,
                           platform_socket_tostring(err));
            }

            listener->wait_coro = mco_running();
            loop_post(listener->loop, _tcp_listener_arm_io_cb, listener);
            mco_yield(mco_running());
            continue;
        }

        xylem_tcp_conn_t* tcp =
            _tcp_conn_alloc(listener->loop, fd, listener->max_read_buf);
        if (!tcp) {
            platform_socket_close(fd);
            continue;
        }

        tcp->read_timeout_ms = listener->read_timeout_ms;
        tcp->write_timeout_ms = listener->write_timeout_ms;

        socklen_t peer_len = sizeof(tcp->peer_addr.storage);
        getpeername(
            fd, (struct sockaddr*)&tcp->peer_addr.storage, &peer_len);

        xylem_logi("tcp listener fd=%d accepted conn fd=%d",
                   (int)listener->fd, (int)fd);
        return tcp;
    }
}

void xylem_tcp_close_listener(xylem_tcp_listener_t* listener) {
    if (listener->closing) {
        return;
    }

    xylem_logi("tcp listener fd=%d closing", (int)listener->fd);
    listener->closing = true;

    if (listener->wait_coro) {
        mco_coro* co = listener->wait_coro;
        listener->wait_coro = NULL;
        scheduler_schedule(runtime_get_scheduler(), co);
    }

    loop_destroy_io(listener->io);
    listener->io = NULL;
    platform_socket_close(listener->fd);
    free(listener);
}

static void _tcp_conn_destroy_cb(
    loop_t* loop, loop_post_t* req, void* ud) {
    (void)loop; (void)req;
    xylem_tcp_conn_t* tcp = (xylem_tcp_conn_t*)ud;
    scheduler_t* sched = runtime_get_scheduler();

    if (tcp->rd_timer) {
        loop_destroy_timer(tcp->rd_timer);
    }
    if (tcp->wr_timer) {
        loop_destroy_timer(tcp->wr_timer);
    }
    if (tcp->read_coro) {
        mco_coro* co = _tcp_io_wake(&tcp->rd_state, &tcp->read_coro);
        if (co) {
            scheduler_schedule(sched, co);
        }
    }
    if (tcp->write_coro) {
        mco_coro* co = _tcp_io_wake(&tcp->wr_state, &tcp->write_coro);
        if (co) {
            scheduler_schedule(sched, co);
        }
    }
    if (tcp->io) {
        loop_destroy_io(tcp->io);
    }
    free(tcp->read_buf);
    shutdown(tcp->fd, PLATFORM_SHUT_WR);
    platform_socket_close(tcp->fd);
    free(tcp);
}

void xylem_tcp_close(xylem_tcp_conn_t* tcp) {
    if (tcp->closed) {
        return;
    }
    tcp->closed = true;
    loop_post(tcp->loop, _tcp_conn_destroy_cb, tcp);
}

int xylem_tcp_get_error(xylem_tcp_conn_t* tcp) {
    return tcp->last_error;
}

int xylem_tcp_remote_addr(
    xylem_tcp_conn_t* tcp,
    char* host,
    size_t host_len,
    uint16_t* port) {
    return addr_ntop(&tcp->peer_addr, host, host_len, port);
}

void* xylem_tcp_get_userdata(xylem_tcp_conn_t* tcp) {
    return tcp->userdata;
}

void xylem_tcp_set_userdata(xylem_tcp_conn_t* tcp, void* ud) {
    tcp->userdata = ud;
}

void* xylem_tcp_listener_get_userdata(xylem_tcp_listener_t* listener) {
    return listener->userdata;
}

void xylem_tcp_listener_set_userdata(
    xylem_tcp_listener_t* listener,
    void* ud) {
    listener->userdata = ud;
}
