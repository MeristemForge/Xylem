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

#include "net/addr.h"
#include "net/framing.h"
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
    xylem_framing_opts_t frame_opts;
    framing_t        framing;
    _Atomic int32_t        refcnt;
    _Atomic bool           closed;
};

struct xylem_tcp_listener_s {
    iowait_t*       waiter;
    platform_sock_t fd;
    size_t          max_read_buf;
    _Atomic int32_t refcnt;
    _Atomic bool    closed;
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
    framing_deinit(&tcp->framing);
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

static int64_t _tcp_raw_recv(xylem_tcp_conn_t* tcp, void* buf, size_t len);

static int64_t _tcp_raw_recv_adapter(void* ctx, void* buf, size_t len) {
    return _tcp_raw_recv((xylem_tcp_conn_t*)ctx, buf, len);
}

static int  _tcp_raw_send(xylem_tcp_conn_t* tcp, const void* data, size_t len);

static int _tcp_raw_send_adapter(void* ctx, const void* data, size_t len) {
    return _tcp_raw_send((xylem_tcp_conn_t*)ctx, data, len);
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
    framing_init(
        &tcp->framing, _tcp_raw_recv_adapter, _tcp_raw_send_adapter,
        tcp, (int)fd, buf_cap);

    _tcp_conn_ref(tcp);
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
            xylem_loge("tcp fd=%d recv error: %s",
                       (int)tcp->fd, platform_socket_tostring(err));
            return -1;
        }

        iowait_result_t r = iowait_read(tcp->waiter);
        if (r != IOWAIT_READY
            || atomic_load_explicit(&tcp->closed, memory_order_acquire)) {
            return -1;
        }
    }
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
            xylem_loge("tcp fd=%d send error: %s",
                       (int)tcp->fd, platform_socket_tostring(err));
            return -1;
        }

        iowait_result_t r = iowait_write(tcp->waiter);
        if (r != IOWAIT_READY
            || atomic_load_explicit(&tcp->closed, memory_order_acquire)) {
            return -1;
        }
    }
    return 0;
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

    bool connected = false;
    platform_sock_t fd = platform_socket_dial(
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

        if (r == IOWAIT_TIMEOUT) {
            xylem_loge("tcp dial: connect timeout for %s:%u", host, port);
            xylem_tcp_close(tcp);
            return NULL;
        }
        if (r == IOWAIT_CLOSED) {
            xylem_tcp_close(tcp);
            return NULL;
        }

        int32_t   err    = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen);
        if (err != 0) {
            xylem_loge("tcp dial fd=%d connect error=%d (%s)",
                       (int)fd,
                       err,
                       platform_socket_tostring(err));
            xylem_tcp_close(tcp);
            return NULL;
        }
    }

    return tcp;
}

void xylem_tcp_set_framing(
    xylem_tcp_conn_t* tcp, xylem_framing_opts_t* opts) {
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
    int64_t ret = framing_recv(&tcp->framing, &tcp->frame_opts, buf, len);
    _tcp_conn_unref(tcp);
    return ret;
}

int xylem_tcp_send(xylem_tcp_conn_t* tcp, const void* data, size_t len) {
    _tcp_conn_ref(tcp);
    int ret = framing_send(&tcp->framing, &tcp->frame_opts, data, len);
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

    _tcp_listener_ref(listener);
    return listener;
}

xylem_tcp_conn_t* xylem_tcp_accept(xylem_tcp_listener_t* listener) {
    _tcp_listener_ref(listener);

    xylem_tcp_conn_t* result = NULL;
    uint64_t          backoff_ms = 5;
    int               retries   = 0;

    for (;;) {
        if (atomic_load_explicit(&listener->closed, memory_order_acquire)) {
            break;
        }

        platform_sock_t fd = platform_socket_accept(listener->fd, true);
        if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN
                || err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                if (iowait_read(listener->waiter) != IOWAIT_READY) {
                    break;
                }
                continue;
            }

            xylem_logw("tcp listener fd=%d accept error=%d (%s)",
                       (int)listener->fd,
                       err,
                       platform_socket_tostring(err));
            if (++retries > 8) {
                break;
            }
            runtime_sleep(backoff_ms);
            if (backoff_ms < 1000) {
                backoff_ms *= 2;
            }
            continue;
        }

        backoff_ms = 5;
        retries    = 0;

        xylem_tcp_conn_t* tcp = _tcp_conn_alloc(fd, listener->max_read_buf);
        if (!tcp) {
            platform_socket_close(fd);
            break;
        }

        socklen_t peer_len = sizeof(tcp->peer_addr.storage);
        getpeername(
            fd, (struct sockaddr*)&tcp->peer_addr.storage, &peer_len);

        result = tcp;
        break;
    }

    _tcp_listener_unref(listener);
    return result;
}

void xylem_tcp_close_listener(xylem_tcp_listener_t* listener) {
    if (atomic_exchange(&listener->closed, true)) {
        return;
    }

    /**
     * Wake any accept coroutine; it will observe `closed`, drop its
     * reference, and the last unref triggers teardown.
     */
    iowait_close(listener->waiter);
    _tcp_listener_unref(listener);
}

void xylem_tcp_close(xylem_tcp_conn_t* tcp) {
    if (atomic_exchange(&tcp->closed, true)) {
        return;
    }
    iowait_close(tcp->waiter);
    _tcp_conn_unref(tcp);
}

int xylem_tcp_remote_addr(
    xylem_tcp_conn_t* tcp,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    return addr_ntop(&tcp->peer_addr, host, host_len, port);
}

int xylem_tcp_local_addr(
    xylem_tcp_conn_t* tcp,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    addr_t addr;
    socklen_t len = sizeof(addr.storage);
    if (getsockname(tcp->fd, (struct sockaddr*)&addr.storage, &len) != 0) {
        return -1;
    }
    return addr_ntop(&addr, host, host_len, port);
}

int xylem_tcp_listener_addr(
    xylem_tcp_listener_t* ln,
    char*                 host,
    size_t                host_len,
    uint16_t*             port) {
    addr_t addr;
    socklen_t len = sizeof(addr.storage);
    if (getsockname(ln->fd, (struct sockaddr*)&addr.storage, &len) != 0) {
        return -1;
    }
    return addr_ntop(&addr, host, host_len, port);
}

int xylem_tcp_shutdown_wr(xylem_tcp_conn_t* tcp) {
    return shutdown(tcp->fd, PLATFORM_SHUT_WR) == 0 ? 0 : -1;
}

int xylem_tcp_shutdown_rd(xylem_tcp_conn_t* tcp) {
    return shutdown(tcp->fd, PLATFORM_SHUT_RD) == 0 ? 0 : -1;
}
