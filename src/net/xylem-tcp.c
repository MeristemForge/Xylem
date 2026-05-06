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
#include "xylem/xylem-utils.h"

#include "addr.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"
#include "runtime/runtime.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_READ_BUF_SIZE 65536

struct xylem_tcp_conn_s {
    iowait_t*              waiter;
    platform_sock_t        fd;
    addr_t                 peer_addr;
    xylem_tcp_frame_opts_t frame_opts;
    char*                  read_buf;
    size_t                 read_buf_cap;
    size_t                 read_buf_pos;
    size_t                 read_buf_len;
    int32_t                last_error;
    _Atomic int32_t        refcnt;
    _Atomic bool           closed;
};

struct xylem_tcp_listener_s {
    iowait_t*       waiter;
    platform_sock_t fd;
    size_t          max_read_buf;
    _Atomic int32_t refcnt;
    _Atomic bool    closing;
};

static void _tcp_conn_ref(xylem_tcp_conn_t* tcp) {
    atomic_fetch_add_explicit(&tcp->refcnt, 1, memory_order_relaxed);
}

static void _tcp_conn_unref(xylem_tcp_conn_t* tcp) {
    if (atomic_fetch_sub_explicit(&tcp->refcnt, 1, memory_order_acq_rel)
        != 1) {
        return;
    }
    if (tcp->waiter) {
        iowait_destroy(tcp->waiter);
    }
    if (tcp->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        shutdown(tcp->fd, PLATFORM_SHUT_WR);
        platform_socket_close(tcp->fd);
    }
    free(tcp->read_buf);
    free(tcp);
}

static void _tcp_listener_ref(xylem_tcp_listener_t* ln) {
    atomic_fetch_add_explicit(&ln->refcnt, 1, memory_order_relaxed);
}

static void _tcp_listener_unref(xylem_tcp_listener_t* ln) {
    if (atomic_fetch_sub_explicit(&ln->refcnt, 1, memory_order_acq_rel)
        != 1) {
        return;
    }
    if (ln->waiter) {
        iowait_destroy(ln->waiter);
    }
    if (ln->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(ln->fd);
    }
    free(ln);
}

static xylem_tcp_conn_t* _tcp_conn_alloc(
    platform_sock_t fd, size_t max_read_buf) {
    xylem_tcp_conn_t* tcp
        = (xylem_tcp_conn_t*)calloc(1, sizeof(xylem_tcp_conn_t));
    if (!tcp) {
        return NULL;
    }

    tcp->fd     = fd;
    tcp->waiter = iowait_create(fd);
    if (!tcp->waiter) {
        free(tcp);
        return NULL;
    }

    size_t buf_cap = max_read_buf > 0 ? max_read_buf : DEFAULT_READ_BUF_SIZE;
    tcp->read_buf = (char*)malloc(buf_cap);
    if (!tcp->read_buf) {
        iowait_destroy(tcp->waiter);
        free(tcp);
        return NULL;
    }
    tcp->read_buf_cap = buf_cap;

    platform_socket_enable_nodelay(fd, true);
    platform_socket_enable_keepalive(fd, true);
    atomic_store_explicit(&tcp->refcnt, 1, memory_order_relaxed);
    return tcp;
}

static int64_t _tcp_raw_recv(xylem_tcp_conn_t* tcp, void* buf, size_t len) {
    if (atomic_load_explicit(&tcp->closed, memory_order_acquire)) {
        return -1;
    }

    for (;;) {
        ssize_t n = platform_socket_recv(tcp->fd, buf, (int)len);
        if (n > 0) {
            return n;
        }
        if (n == 0) {
            return 0;
        }

        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN
            && err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            tcp->last_error = err;
            return -1;
        }

        iowait_result_t r = iowait_read(tcp->waiter);
        if (r != IOWAIT_READY
            || atomic_load_explicit(&tcp->closed, memory_order_acquire)) {
            tcp->last_error = (r == IOWAIT_TIMEOUT)
                ? PLATFORM_SO_ERROR_ETIMEDOUT
                : PLATFORM_SO_ERROR_ECONNRESET;
            return -1;
        }
    }
}

static int _tcp_read_exact(xylem_tcp_conn_t* tcp, void* buf, size_t len) {
    char*  ptr = (char*)buf;
    size_t rem = len;

    while (rem > 0) {
        size_t avail = tcp->read_buf_len - tcp->read_buf_pos;
        if (avail > 0) {
            size_t copy = avail < rem ? avail : rem;
            memcpy(ptr, tcp->read_buf + tcp->read_buf_pos, copy);
            tcp->read_buf_pos += copy;
            ptr += copy;
            rem -= copy;
            continue;
        }

        tcp->read_buf_pos = 0;
        tcp->read_buf_len = 0;

        int64_t n = _tcp_raw_recv(tcp, tcp->read_buf, tcp->read_buf_cap);
        if (n <= 0) {
            return -1;
        }
        tcp->read_buf_len = (size_t)n;
    }
    return 0;
}

static int64_t
_tcp_buffered_read(xylem_tcp_conn_t* tcp, void* buf, size_t len) {
    size_t avail = tcp->read_buf_len - tcp->read_buf_pos;
    if (avail > 0) {
        size_t copy = avail < len ? avail : len;
        memcpy(buf, tcp->read_buf + tcp->read_buf_pos, copy);
        tcp->read_buf_pos += copy;
        return (int64_t)copy;
    }

    if (len >= tcp->read_buf_cap) {
        return _tcp_raw_recv(tcp, buf, len);
    }

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

static int64_t
_tcp_recv_fixed(xylem_tcp_conn_t* tcp, void* buf, size_t len) {
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

static int64_t
_tcp_recv_length(xylem_tcp_conn_t* tcp, void* buf, size_t len) {
    uint8_t  hdr[16];
    uint32_t hdr_sz = tcp->frame_opts.length.header_size;

    if (hdr_sz > sizeof(hdr)) {
        tcp->last_error = -1;
        return -1;
    }

    if (_tcp_read_exact(tcp, hdr, hdr_sz) != 0) {
        return -1;
    }

    uint64_t body_len = 0;
    uint8_t* field    = hdr + tcp->frame_opts.length.field_offset;

    if (tcp->frame_opts.length.big_endian) {
        for (uint32_t i = 0; i < tcp->frame_opts.length.field_size; i++) {
            body_len = (body_len << 8) | field[i];
        }
    } else {
        for (uint32_t i = 0; i < tcp->frame_opts.length.field_size; i++) {
            body_len |= (uint64_t)field[i] << (i * 8);
        }
    }

    int64_t adjusted
        = (int64_t)body_len + tcp->frame_opts.length.adjustment;
    if (adjusted < 0) {
        tcp->last_error = -1;
        return -1;
    }

    size_t payload_len = (size_t)adjusted;
    if (payload_len > len) {
        tcp->last_error = -1;
        return -1;
    }

    if (payload_len > 0 && _tcp_read_exact(tcp, buf, payload_len) != 0) {
        return -1;
    }
    return (int64_t)payload_len;
}

static int64_t
_tcp_recv_delimiter(xylem_tcp_conn_t* tcp, void* buf, size_t len) {
    const char* delim     = tcp->frame_opts.delimiter.delim;
    size_t      delim_len = tcp->frame_opts.delimiter.delim_len;
    if (delim_len == 0) {
        delim_len = strlen(delim);
    }

    char*  dst = (char*)buf;
    size_t pos = 0;

    while (pos < len) {
        size_t avail = tcp->read_buf_len - tcp->read_buf_pos;
        if (avail == 0) {
            tcp->read_buf_pos = 0;
            tcp->read_buf_len = 0;
            int64_t n
                = _tcp_raw_recv(tcp, tcp->read_buf, tcp->read_buf_cap);
            if (n <= 0) {
                return -1;
            }
            tcp->read_buf_len = (size_t)n;
            avail             = (size_t)n;
        }

        char* src = tcp->read_buf + tcp->read_buf_pos;
        for (size_t i = 0; i < avail && pos < len; i++) {
            dst[pos++] = src[i];
            tcp->read_buf_pos++;

            if (pos >= delim_len
                && memcmp(dst + pos - delim_len, delim, delim_len) == 0) {
                pos -= delim_len;
                dst[pos] = '\0';
                return (int64_t)pos;
            }
        }
    }

    tcp->last_error = -1;
    return -1;
}

static int
_tcp_raw_send(xylem_tcp_conn_t* tcp, const void* data, size_t len) {
    if (atomic_load_explicit(&tcp->closed, memory_order_acquire)) {
        return -1;
    }

    const char* ptr = (const char*)data;
    size_t      rem = len;

    while (rem > 0) {
        ssize_t n = platform_socket_send(tcp->fd, ptr, (int)rem);
        if (n > 0) {
            ptr += n;
            rem -= (size_t)n;
            continue;
        }

        int err = platform_socket_get_lasterror();
        if (err != PLATFORM_SO_ERROR_EAGAIN
            && err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
            tcp->last_error = err;
            return -1;
        }

        iowait_result_t r = iowait_write(tcp->waiter);
        if (r != IOWAIT_READY
            || atomic_load_explicit(&tcp->closed, memory_order_acquire)) {
            tcp->last_error = (r == IOWAIT_TIMEOUT)
                ? PLATFORM_SO_ERROR_ETIMEDOUT
                : PLATFORM_SO_ERROR_ECONNRESET;
            return -1;
        }
    }
    return 0;
}

static int
_tcp_send_length(xylem_tcp_conn_t* tcp, const void* data, size_t len) {
    uint8_t  hdr[16];
    uint32_t hdr_sz = tcp->frame_opts.length.header_size;

    if (hdr_sz > sizeof(hdr)) {
        return -1;
    }

    int64_t wire_len = (int64_t)len - tcp->frame_opts.length.adjustment;
    if (wire_len < 0) {
        return -1;
    }

    memset(hdr, 0, hdr_sz);
    uint8_t* field = hdr + tcp->frame_opts.length.field_offset;
    uint64_t val   = (uint64_t)wire_len;

    if (tcp->frame_opts.length.big_endian) {
        for (int32_t i = (int32_t)tcp->frame_opts.length.field_size - 1;
             i >= 0;
             i--) {
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
    return _tcp_raw_send(tcp, data, len);
}

xylem_tcp_conn_t* xylem_tcp_dial(
    const char*       host,
    uint16_t          port,
    uint64_t          connect_timeout_ms,
    xylem_tcp_opts_t* opts) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    const char* dial_host = host;
    char        resolved_ip[INET6_ADDRSTRLEN];
    addr_t      resolved_addr;

    if (addr_pton(host, port, &resolved_addr) != 0) {
        addr_t* addrs = NULL;
        size_t  count = 0;
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

    bool            connected = false;
    platform_sock_t fd        = platform_socket_dial(
        dial_host, port_str, SOCK_STREAM, &connected, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge(
            "tcp dial: socket creation failed for %s:%s", host, port_str);
        return NULL;
    }

    if (opts && opts->disable_mss_clamp) {
        platform_socket_enable_mss_clamp(fd, false);
    }

    size_t            max_buf = opts ? opts->max_read_buf : 0;
    xylem_tcp_conn_t* tcp     = _tcp_conn_alloc(fd, max_buf);
    if (!tcp) {
        platform_socket_close(fd);
        return NULL;
    }

    tcp->peer_addr = resolved_addr;

    xylem_logd("tcp dial fd=%d connected=%d", (int)fd, connected);
    if (!connected) {
        /**
         * Connect completion surfaces as writability on the fd. Apply
         * the connect timeout as a one-shot write deadline; it is
         * cleared after dial returns so subsequent xylem_tcp_send calls
         * start with no deadline.
         */
        if (connect_timeout_ms > 0) {
            uint64_t deadline
                = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                  + connect_timeout_ms;
            iowait_set_wr_deadline(tcp->waiter, deadline);
        }
        iowait_result_t r = iowait_write(tcp->waiter);
        iowait_set_wr_deadline(tcp->waiter, 0);

        if (r != IOWAIT_READY) {
            tcp->last_error = PLATFORM_SO_ERROR_ETIMEDOUT;
            xylem_logd("tcp dial fd=%d connect timed out", (int)fd);
            xylem_tcp_close(tcp);
            return NULL;
        }

        int32_t   err    = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen);
        if (err != 0) {
            tcp->last_error = err;
            xylem_loge("tcp dial fd=%d connect error=%d (%s)",
                       (int)fd,
                       err,
                       platform_socket_tostring(err));
            xylem_tcp_close(tcp);
            return NULL;
        }
    }

    xylem_logi(
        "tcp conn fd=%d connected to %s:%s", (int)fd, host, port_str);
    return tcp;
}

void xylem_tcp_set_framing(
    xylem_tcp_conn_t* tcp, xylem_tcp_frame_opts_t* opts) {
    if (opts) {
        tcp->frame_opts = *opts;
    } else {
        memset(&tcp->frame_opts, 0, sizeof(tcp->frame_opts));
    }
}

void xylem_tcp_set_read_deadline(
    xylem_tcp_conn_t* tcp, uint64_t deadline_ms) {
    iowait_set_rd_deadline(tcp->waiter, deadline_ms);
}

void xylem_tcp_set_write_deadline(
    xylem_tcp_conn_t* tcp, uint64_t deadline_ms) {
    iowait_set_wr_deadline(tcp->waiter, deadline_ms);
}

int64_t
xylem_tcp_recv(xylem_tcp_conn_t* tcp, void* buf, size_t len) {
    _tcp_conn_ref(tcp);
    int64_t ret;
    switch (tcp->frame_opts.type) {
    case XYLEM_TCP_FRAME_NONE:
        ret = _tcp_buffered_read(tcp, buf, len);
        break;
    case XYLEM_TCP_FRAME_FIXED:
        ret = _tcp_recv_fixed(tcp, buf, len);
        break;
    case XYLEM_TCP_FRAME_LENGTH:
        ret = _tcp_recv_length(tcp, buf, len);
        break;
    case XYLEM_TCP_FRAME_DELIMITER:
        ret = _tcp_recv_delimiter(tcp, buf, len);
        break;
    default:
        ret = -1;
        break;
    }
    _tcp_conn_unref(tcp);
    return ret;
}

int xylem_tcp_send(xylem_tcp_conn_t* tcp, const void* data, size_t len) {
    _tcp_conn_ref(tcp);
    int ret;
    switch (tcp->frame_opts.type) {
    case XYLEM_TCP_FRAME_LENGTH:
        ret = _tcp_send_length(tcp, data, len);
        break;
    default:
        ret = _tcp_raw_send(tcp, data, len);
        break;
    }
    _tcp_conn_unref(tcp);
    return ret;
}

xylem_tcp_listener_t* xylem_tcp_listen(
    const char* host, uint16_t port, xylem_tcp_opts_t* opts) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd
        = platform_socket_listen(host, port_str, SOCK_STREAM, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("tcp listen: failed for %s:%s", host, port_str);
        return NULL;
    }

    if (opts && opts->disable_mss_clamp) {
        platform_socket_enable_mss_clamp(fd, false);
    }

    xylem_tcp_listener_t* listener = (xylem_tcp_listener_t*)calloc(
        1, sizeof(xylem_tcp_listener_t));
    if (!listener) {
        platform_socket_close(fd);
        return NULL;
    }

    listener->fd = fd;
    if (opts) {
        listener->max_read_buf = opts->max_read_buf;
    }
    listener->waiter = iowait_create(fd);
    if (!listener->waiter) {
        platform_socket_close(fd);
        free(listener);
        return NULL;
    }

    atomic_store_explicit(&listener->refcnt, 1, memory_order_relaxed);

    xylem_logi("tcp listener fd=%d listening on %s:%s",
               (int)fd,
               host,
               port_str);
    return listener;
}

xylem_tcp_conn_t* xylem_tcp_accept(xylem_tcp_listener_t* listener) {
    _tcp_listener_ref(listener);

    xylem_tcp_conn_t* result = NULL;
    for (;;) {
        if (atomic_load_explicit(&listener->closing, memory_order_acquire)) {
            break;
        }

        platform_sock_t fd = platform_socket_accept(listener->fd, true);
        if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN
                || err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                /* Expected: no pending connection. Park until readable. */
                if (iowait_read(listener->waiter) != IOWAIT_READY) {
                    break;
                }
                continue;
            }

            /**
             * Non-EAGAIN error: could be transient (ECONNABORTED, EINTR,
             * per-connection SSL/TCP reset) or resource exhaustion
             * (EMFILE, ENFILE). In the latter case the listen fd stays
             * readable and calling iowait_read returns immediately, so
             * looping would burn 100% CPU. Sleep briefly as backoff
             * before retrying; under fd exhaustion this lets other
             * descriptors close, and under transient errors it is cheap.
             */
            xylem_logw("tcp listener fd=%d accept error=%d (%s)",
                       (int)listener->fd,
                       err,
                       platform_socket_tostring(err));
            runtime_sleep(10);
            continue;
        }

        xylem_tcp_conn_t* tcp = _tcp_conn_alloc(fd, listener->max_read_buf);
        if (!tcp) {
            platform_socket_close(fd);
            continue;
        }

        socklen_t peer_len = sizeof(tcp->peer_addr.storage);
        getpeername(
            fd, (struct sockaddr*)&tcp->peer_addr.storage, &peer_len);

        xylem_logi("tcp listener fd=%d accepted conn fd=%d",
                   (int)listener->fd,
                   (int)fd);
        result = tcp;
        break;
    }

    _tcp_listener_unref(listener);
    return result;
}

void xylem_tcp_close_listener(xylem_tcp_listener_t* listener) {
    if (atomic_exchange(&listener->closing, true)) {
        return;
    }

    xylem_logi("tcp listener fd=%d closing", (int)listener->fd);

    /**
     * Wake any accept coroutine; it will observe `closing`, drop its
     * reference, and the last unref triggers teardown.
     */
    iowait_close(listener->waiter);
    _tcp_listener_unref(listener);
}

void xylem_tcp_close(xylem_tcp_conn_t* tcp) {
    if (atomic_exchange(&tcp->closed, true)) {
        return;
    }

    /**
     * Wake any in-flight recv/send so they observe `closed` and return.
     * The actual teardown (fd close, iowait_destroy, free) happens in
     * _tcp_conn_unref() once the last in-flight call drops its reference.
     */
    iowait_close(tcp->waiter);
    _tcp_conn_unref(tcp);
}

int xylem_tcp_get_error(xylem_tcp_conn_t* tcp) {
    return tcp->last_error;
}

int xylem_tcp_remote_addr(
    xylem_tcp_conn_t* tcp,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    return addr_ntop(&tcp->peer_addr, host, host_len, port);
}
