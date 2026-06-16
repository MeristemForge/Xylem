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

#include "net/tcp/stream.h"

#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "runtime/iowait.h"
#include "runtime/runtime.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

struct stream_s {
    iowait_t*       waiter;
    platform_sock_t fd;
    addr_t          peer_addr;
    bool            peer_addr_valid;
    _Atomic int32_t refcnt;
    _Atomic bool    closed;
};

struct listener_s {
    iowait_t*       waiter;
    platform_sock_t fd;
    _Atomic int32_t refcnt;
    _Atomic bool    closed;
};

static void _stream_ref(stream_t* stream) {
    atomic_fetch_add_explicit(&stream->refcnt, 1, memory_order_relaxed);
}

static void _stream_unref(stream_t* stream) {
    if (atomic_fetch_sub_explicit(&stream->refcnt, 1, memory_order_acq_rel)
        != 1) {
        return;
    }

    if (stream->waiter) {
        /**
         * Disarm any in-flight deadline timer before teardown. iowait
         * close/destroy do not cancel timers, and an armed timer holds
         * an iowait reference until the stale deadline fires.
         */
        iowait_set_rd_deadline(stream->waiter, 0);
        iowait_set_wr_deadline(stream->waiter, 0);
        iowait_destroy(stream->waiter);
    }
    if (stream->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        shutdown(stream->fd, PLATFORM_SHUT_WR);
        platform_socket_close(stream->fd);
    }
    free(stream);
}

static void _listener_ref(listener_t* listener) {
    atomic_fetch_add_explicit(&listener->refcnt, 1, memory_order_relaxed);
}

static void _listener_unref(listener_t* listener) {
    if (atomic_fetch_sub_explicit(&listener->refcnt, 1, memory_order_acq_rel)
        != 1) {
        return;
    }

    if (listener->waiter) {
        iowait_destroy(listener->waiter);
    }
    if (listener->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(listener->fd);
    }
    free(listener);
}

stream_t* stream_from_fd(platform_sock_t fd) {
    stream_t* stream = (stream_t*)calloc(1, sizeof(stream_t));
    if (!stream) {
        return NULL;
    }

    stream->fd     = fd;
    stream->waiter = iowait_create(fd);
    if (!stream->waiter) {
        free(stream);
        return NULL;
    }
    iowait_enable_readiness_cache(stream->waiter);

    _stream_ref(stream);
    return stream;
}

stream_t* stream_dial(
    const char* host,
    uint16_t    port,
    uint64_t    connect_timeout_ms,
    bool        enable_mss_clamp) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    const char* dial_host = host;
    char        resolved_ip[INET6_ADDRSTRLEN];
    addr_t      resolved_addr;

    if (addr_pton(host, port, &resolved_addr) != 0) {
        addr_t* addrs = NULL;
        size_t  count = 0;
        if (addr_resolve(host, port, connect_timeout_ms, &addrs, &count) != 0
            || count == 0) {
            xylem_loge("<stream> dial dns failed host=%s", host);
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
        dial_host,
        port_str,
        SOCK_STREAM,
        &connected,
        true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge(
            "<stream> dial socket failed host=%s port=%s",
            host,
            port_str);
        return NULL;
    }

    if (enable_mss_clamp) {
        platform_socket_enable_mss_clamp(fd, true);
    }

    stream_t* stream = stream_from_fd(fd);
    if (!stream) {
        platform_socket_close(fd);
        return NULL;
    }

    stream->peer_addr       = resolved_addr;
    stream->peer_addr_valid = true;

    if (!connected) {
        /**
         * Connect completion surfaces as writability on the fd. Apply
         * the connect timeout as a one-shot write deadline, then clear it
         * so later writes start with no inherited deadline.
         */
        if (connect_timeout_ms > 0) {
            uint64_t deadline
                = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                  + connect_timeout_ms;
            iowait_set_wr_deadline(stream->waiter, deadline);
        }
        iowait_result_t r = iowait_write(stream->waiter);
        iowait_set_wr_deadline(stream->waiter, 0);

        if (r == IOWAIT_TIMEOUT) {
            xylem_loge(
                "<stream> dial connect timeout host=%s port=%u",
                host,
                port);
            stream_interrupt(stream);
            stream_release(stream);
            return NULL;
        }
        if (r == IOWAIT_CLOSED) {
            stream_interrupt(stream);
            stream_release(stream);
            return NULL;
        }

        int32_t   err    = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &errlen);
        if (err != 0) {
            xylem_loge(
                "<stream> dial connect failed fd=%d err=%d (%s)",
                (int)fd,
                err,
                platform_socket_tostring(err));
            stream_interrupt(stream);
            stream_release(stream);
            return NULL;
        }
    }

    return stream;
}

void stream_interrupt(stream_t* stream) {
    if (atomic_exchange(&stream->closed, true)) {
        return;
    }
    iowait_close(stream->waiter);
}

void stream_release(stream_t* stream) {
    _stream_unref(stream);
}

void stream_set_read_deadline(
    stream_t* stream,
    uint64_t  deadline_ms) {
    iowait_set_rd_deadline(stream->waiter, deadline_ms);
}

void stream_set_write_deadline(
    stream_t* stream,
    uint64_t  deadline_ms) {
    iowait_set_wr_deadline(stream->waiter, deadline_ms);
}

int stream_read(stream_t* stream, void* buf, int len) {
    _stream_ref(stream);
    int ret = -1;

    if (!atomic_load_explicit(&stream->closed, memory_order_acquire)) {
        for (;;) {
            iowait_ready_event_t ev = iowait_read_ready_event(stream->waiter);
            ssize_t n = platform_socket_recv(stream->fd, buf, len);
            if (n >= 0) {
                ret = (int)n;
                break;
            }

            int err = platform_socket_get_lasterror();
            if (err != PLATFORM_SO_ERROR_EAGAIN
                && err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
                xylem_loge(
                    "<stream> read failed fd=%d err=%s",
                    (int)stream->fd,
                    platform_socket_tostring(err));
                break;
            }

            iowait_clear_read_ready(stream->waiter, ev);
            iowait_result_t r = iowait_read(stream->waiter);
            if (r != IOWAIT_READY
                || atomic_load_explicit(&stream->closed, memory_order_acquire)) {
                break;
            }
        }
    }

    _stream_unref(stream);
    return ret;
}

int stream_write(stream_t* stream, const void* data, int len) {
    _stream_ref(stream);
    int ret = -1;

    if (!atomic_load_explicit(&stream->closed, memory_order_acquire)) {
        const char* ptr = (const char*)data;
        int         rem = len;

        iowait_set_write_active(stream->waiter, true);
        while (rem > 0) {
            iowait_ready_event_t ev = iowait_write_ready_event(stream->waiter);
            ssize_t n = platform_socket_send(stream->fd, ptr, rem);
            if (n > 0) {
                ptr += n;
                rem -= (int)n;
                continue;
            }

            int err = platform_socket_get_lasterror();
            if (err != PLATFORM_SO_ERROR_EAGAIN
                && err != PLATFORM_SO_ERROR_EWOULDBLOCK) {
                xylem_loge(
                    "<stream> write failed fd=%d err=%s",
                    (int)stream->fd,
                    platform_socket_tostring(err));
                break;
            }

            iowait_clear_write_ready(stream->waiter, ev);
            iowait_result_t r = iowait_write(stream->waiter);
            if (r != IOWAIT_READY
                || atomic_load_explicit(&stream->closed, memory_order_acquire)) {
                break;
            }
        }
        if (rem == 0) {
            ret = 0;
        }
        iowait_set_write_active(stream->waiter, false);
    }

    _stream_unref(stream);
    return ret;
}

int stream_remote_addr(
    stream_t* stream,
    char*     host,
    size_t    host_len,
    uint16_t* port) {
    if (!stream->peer_addr_valid) {
        socklen_t peer_len = sizeof(stream->peer_addr.storage);
        if (getpeername(
                stream->fd,
                (struct sockaddr*)&stream->peer_addr.storage,
                &peer_len)
            != 0) {
            return -1;
        }
        stream->peer_addr_valid = true;
    }

    return addr_ntop(&stream->peer_addr, host, host_len, port);
}

int stream_local_addr(
    stream_t* stream,
    char*     host,
    size_t    host_len,
    uint16_t* port) {
    addr_t addr;
    socklen_t len = sizeof(addr.storage);
    if (getsockname(stream->fd, (struct sockaddr*)&addr.storage, &len) != 0) {
        return -1;
    }
    return addr_ntop(&addr, host, host_len, port);
}

int stream_shutdown_wr(stream_t* stream) {
    return shutdown(stream->fd, PLATFORM_SHUT_WR) == 0 ? 0 : -1;
}

int stream_shutdown_rd(stream_t* stream) {
    return shutdown(stream->fd, PLATFORM_SHUT_RD) == 0 ? 0 : -1;
}

platform_sock_t stream_fd(stream_t* stream) {
    return stream->fd;
}

listener_t* listener_listen(
    const char* host,
    uint16_t    port,
    bool        enable_mss_clamp) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    platform_sock_t fd
        = platform_socket_listen(host, port_str, SOCK_STREAM, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("<stream> listen failed host=%s port=%s", host, port_str);
        return NULL;
    }

    if (enable_mss_clamp) {
        platform_socket_enable_mss_clamp(fd, true);
    }

    listener_t* listener
        = (listener_t*)calloc(1, sizeof(listener_t));
    if (!listener) {
        platform_socket_close(fd);
        return NULL;
    }

    listener->fd     = fd;
    listener->waiter = iowait_create(fd);
    if (!listener->waiter) {
        platform_socket_close(fd);
        free(listener);
        return NULL;
    }

    _listener_ref(listener);
    return listener;
}

stream_t* listener_accept(listener_t* listener) {
    _listener_ref(listener);

    stream_t* result     = NULL;
    uint64_t  backoff_ms = 5;
    int       retries    = 0;

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

            xylem_loge(
                "<stream> accept failed fd=%d err=%d (%s)",
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

        stream_t* stream = stream_from_fd(fd);
        if (!stream) {
            platform_socket_close(fd);
            break;
        }

        result = stream;
        break;
    }

    _listener_unref(listener);
    return result;
}

void listener_interrupt(listener_t* listener) {
    if (atomic_exchange(&listener->closed, true)) {
        return;
    }

    iowait_close(listener->waiter);
}

void listener_release(listener_t* listener) {
    _listener_unref(listener);
}

int listener_addr(
    listener_t* listener,
    char*       host,
    size_t      host_len,
    uint16_t*   port) {
    addr_t addr;
    socklen_t len = sizeof(addr.storage);
    if (getsockname(listener->fd, (struct sockaddr*)&addr.storage, &len) != 0) {
        return -1;
    }
    return addr_ntop(&addr, host, host_len, port);
}
