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

#include "net/stream.h"

#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "net/addr.h"
#include "runtime/iowait.h"
#include "runtime/runtime.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define STREAM_ACCEPT_INITIAL_BACKOFF_MS 5
#define STREAM_ACCEPT_MAX_BACKOFF_MS     1000
#define STREAM_ACCEPT_BACKOFF_SLICE_MS   10

struct stream_s {
    iowait_t*       waiter;
    platform_sock_t fd;
    _Atomic bool    rd_shutdown;
    _Atomic bool    closed;
};

struct listener_s {
    iowait_t*       waiter;
    platform_sock_t fd;
    _Atomic bool    closed;
};

static int _stream_wait_dial(
    stream_t* stream,
    uint64_t  deadline_ms) {
    if (deadline_ms > 0) {
        iowait_set_wr_deadline(stream->waiter, deadline_ms);
    }
    iowait_result_t r = iowait_write(stream->waiter);
    iowait_set_wr_deadline(stream->waiter, 0);

    if (r != IOWAIT_READY) {
        xylem_loge(
            "<stream> connect wait failed fd=%d result=%d",
            (int)stream->fd,
            (int)r);
        return -1;
    }

    int       err    = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(
            stream->fd,
            SOL_SOCKET,
            SO_ERROR,
            (char*)&err,
            &errlen)
        != 0) {
        err = platform_socket_get_lasterror();
        xylem_loge(
            "<stream> connect getsockopt failed fd=%d err=%d (%s)",
            (int)stream->fd,
            err,
            platform_socket_tostring(err));
        return -1;
    }
    if (err != 0) {
        xylem_loge(
            "<stream> connect failed fd=%d err=%d (%s)",
            (int)stream->fd,
            err,
            platform_socket_tostring(err));
        return -1;
    }
    return 0;
}

static bool _listener_wait_backoff(
    listener_t* listener,
    uint64_t    backoff_ms) {
    while (backoff_ms > 0 && !atomic_load(&listener->closed)) {
        uint64_t sleep_ms = backoff_ms > STREAM_ACCEPT_BACKOFF_SLICE_MS
            ? STREAM_ACCEPT_BACKOFF_SLICE_MS
            : backoff_ms;
        runtime_sleep(sleep_ms);
        backoff_ms -= sleep_ms;
    }

    return !atomic_load(&listener->closed);
}

static stream_t* _listener_accept(
    listener_t* listener,
    bool        unix_socket) {
    stream_t* stream     = NULL;
    uint64_t  backoff_ms = STREAM_ACCEPT_INITIAL_BACKOFF_MS;

    for (;;) {
        if (atomic_load(&listener->closed)) {
            break;
        }

        platform_sock_t fd = unix_socket
            ? platform_socket_accept_unix(listener->fd, true)
            : platform_socket_accept(listener->fd, true);
        if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            int err = platform_socket_get_lasterror();
            if (atomic_load(&listener->closed)) {
                break;
            }
            if (err == PLATFORM_SO_ERROR_EAGAIN
                || err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                if (iowait_read(listener->waiter) != IOWAIT_READY) {
                    break;
                }
                continue;
            }

            if (!platform_socket_accept_should_retry(err)) {
                xylem_loge(
                    "<stream> accept failed fd=%d err=%d (%s)",
                    (int)listener->fd,
                    err,
                    platform_socket_tostring(err));
                break;
            }
            if (!_listener_wait_backoff(listener, backoff_ms)) {
                break;
            }

            if (backoff_ms < STREAM_ACCEPT_MAX_BACKOFF_MS) {
                backoff_ms *= 2;
                if (backoff_ms > STREAM_ACCEPT_MAX_BACKOFF_MS) {
                    backoff_ms = STREAM_ACCEPT_MAX_BACKOFF_MS;
                }
            }
            continue;
        }

        if (atomic_load(&listener->closed)) {
            platform_socket_close(fd);
            break;
        }

        stream = stream_from_fd(fd);
        if (!stream) {
            platform_socket_close(fd);
            break;
        }

        break;
    }

    return stream;
}

listener_t* listener_from_fd(platform_sock_t fd) {
    listener_t* listener = (listener_t*)calloc(1, sizeof(listener_t));
    if (!listener) {
        return NULL;
    }

    listener->fd     = fd;
    listener->waiter = iowait_create(fd);
    if (!listener->waiter) {
        free(listener);
        return NULL;
    }

    atomic_init(&listener->closed, false);
    return listener;
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

    atomic_init(&stream->rd_shutdown, false);
    atomic_init(&stream->closed, false);
    return stream;
}

stream_t* stream_dial(
    const char* host,
    uint16_t    port,
    uint64_t    connect_timeout_ms,
    bool        enable_mss_clamp) {
    if (!host || !*host) {
        return NULL;
    }

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    uint64_t connect_deadline_ms = 0;
    if (connect_timeout_ms > 0) {
        uint64_t now = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
        connect_deadline_ms = connect_timeout_ms >= UINT64_MAX - now
            ? UINT64_MAX
            : now + connect_timeout_ms;
    }

    addr_t* addrs = NULL;
    size_t  count = 0;
    uint64_t resolve_timeout_ms = connect_deadline_ms == UINT64_MAX
        ? 0
        : connect_timeout_ms;
    if (addr_lookup(
            host,
            port,
            resolve_timeout_ms,
            &addrs,
            &count)
        != 0) {
        xylem_loge("<stream> dial resolve failed host=%s", host);
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        uint64_t attempt_deadline_ms = 0;
        if (connect_deadline_ms > 0) {
            uint64_t now_ms
                = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
            if (now_ms >= connect_deadline_ms) {
                break;
            }

            uint64_t remaining_ms = connect_deadline_ms - now_ms;
            uint64_t attempt_ms   = remaining_ms / (count - i);
            if (attempt_ms == 0) {
                attempt_ms = 1;
            }
            attempt_deadline_ms = now_ms + attempt_ms;
        }

        char resolved_ip[INET6_ADDRSTRLEN];
        if (addr_ntop(&addrs[i], resolved_ip, sizeof(resolved_ip), NULL)
            != 0) {
            continue;
        }

        bool connected = false;
        platform_sock_t fd = platform_socket_dial(
            resolved_ip,
            port_str,
            SOCK_STREAM,
            &connected,
            true,
            enable_mss_clamp);
        if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            continue;
        }

        if (attempt_deadline_ms > 0
            && xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                   >= attempt_deadline_ms) {
            platform_socket_close(fd);
            continue;
        }

        stream_t* stream = stream_from_fd(fd);
        if (!stream) {
            platform_socket_close(fd);
            break;
        }

        if (!connected
            && _stream_wait_dial(stream, attempt_deadline_ms) != 0) {
            stream_destroy(stream);
            continue;
        }

        free(addrs);
        return stream;
    }

    free(addrs);
    return NULL;
}

stream_t* stream_dial_unix(
    const char* path,
    uint64_t    connect_timeout_ms) {
    if (!path) {
        return NULL;
    }

    uint64_t connect_deadline_ms = 0;
    if (connect_timeout_ms > 0) {
        connect_deadline_ms
            = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
              + connect_timeout_ms;
    }

    bool connected = false;
    platform_sock_t fd
        = platform_socket_dial_unix(path, &connected, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("<stream> dial unix failed path=%s", path);
        return NULL;
    }

    if (connect_deadline_ms > 0
        && xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
               >= connect_deadline_ms) {
        xylem_loge("<stream> dial unix timeout path=%s", path);
        platform_socket_close(fd);
        return NULL;
    }

    stream_t* stream = stream_from_fd(fd);
    if (!stream) {
        platform_socket_close(fd);
        return NULL;
    }

    if (!connected
        && _stream_wait_dial(stream, connect_deadline_ms) != 0) {
        stream_destroy(stream);
        return NULL;
    }

    return stream;
}

void stream_close(stream_t* stream) {
    if (!atomic_exchange(&stream->closed, true)) {
        iowait_close(stream->waiter);
    }
}

void stream_destroy(stream_t* stream) {
    if (!stream) {
        return;
    }
    /* Detach iowait before closing the fd so a reused fd has no stale poller state. */
    stream_close(stream);
    if (stream->waiter) {
        iowait_destroy(stream->waiter);
    }
    if (stream->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        shutdown(stream->fd, PLATFORM_SHUT_WR);
        platform_socket_close(stream->fd);
    }
    free(stream);
}

void stream_set_read_deadline(
    stream_t* stream,
    uint64_t  deadline_ms) {
    if (!atomic_load(&stream->closed)) {
        iowait_set_rd_deadline(stream->waiter, deadline_ms);
    }
}

void stream_set_write_deadline(
    stream_t* stream,
    uint64_t  deadline_ms) {
    if (!atomic_load(&stream->closed)) {
        iowait_set_wr_deadline(stream->waiter, deadline_ms);
    }
}

static iowait_result_t _stream_wait(
    stream_t* stream,
    bool      write) {
    if (atomic_load(&stream->closed)
        || (!write && atomic_load(&stream->rd_shutdown))) {
        return IOWAIT_CLOSED;
    }

    iowait_result_t ret = write
        ? iowait_write(stream->waiter)
        : iowait_read(stream->waiter);
    if (atomic_load(&stream->closed)) {
        ret = IOWAIT_CLOSED;
    }

    return ret;
}

int stream_read(
    stream_t* stream,
    void*     buf,
    int       len) {
    if (!buf || len <= 0) {
        return -1;
    }

    if (atomic_load(&stream->closed)
        || atomic_load(&stream->rd_shutdown)
        || iowait_read_deadline_expired(stream->waiter)) {
        return -1;
    }

    ssize_t n = platform_socket_recv(stream->fd, buf, len);
    if (n >= 0) {
        return (int)n;
    }

    int err = platform_socket_get_lasterror();
    if (err == PLATFORM_SO_ERROR_EAGAIN
        || err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
        return STREAM_IO_AGAIN;
    }

    xylem_loge(
        "<stream> read failed fd=%d err=%s",
        (int)stream->fd,
        platform_socket_tostring(err));
    return -1;
}

iowait_result_t stream_wait_read(stream_t* stream) {
    return _stream_wait(stream, false);
}

int stream_write(
    stream_t*   stream,
    const void* data,
    int         len) {
    if (len < 0 || (len > 0 && !data)) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    if (atomic_load(&stream->closed)
        || iowait_write_deadline_expired(stream->waiter)) {
        return -1;
    }

    ssize_t n = platform_socket_send(stream->fd, data, len);
    if (n > 0) {
        return (int)n;
    }

    if (n < 0) {
        int err = platform_socket_get_lasterror();
        if (err == PLATFORM_SO_ERROR_EAGAIN
            || err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
            return STREAM_IO_AGAIN;
        }

        xylem_loge(
            "<stream> write failed fd=%d err=%s",
            (int)stream->fd,
            platform_socket_tostring(err));
    }
    return -1;
}

iowait_result_t stream_wait_write(stream_t* stream) {
    return _stream_wait(stream, true);
}

int stream_remote_addr(
    stream_t* stream,
    char*     host,
    size_t    host_len,
    uint16_t* port) {
    if (atomic_load(&stream->closed)) {
        return -1;
    }

    addr_t    addr     = {0};
    socklen_t peer_len = sizeof(addr.storage);
    if (getpeername(stream->fd, (struct sockaddr*)&addr.storage, &peer_len)
        != 0) {
        return -1;
    }

    return addr_ntop(&addr, host, host_len, port);
}

int stream_local_addr(
    stream_t* stream,
    char*     host,
    size_t    host_len,
    uint16_t* port) {
    if (atomic_load(&stream->closed)) {
        return -1;
    }

    addr_t    addr = {0};
    socklen_t len = sizeof(addr.storage);
    if (getsockname(stream->fd, (struct sockaddr*)&addr.storage, &len) != 0) {
        return -1;
    }
    return addr_ntop(&addr, host, host_len, port);
}

int stream_shutdown_wr(stream_t* stream) {
    if (atomic_load(&stream->closed)) {
        return -1;
    }

    return shutdown(stream->fd, PLATFORM_SHUT_WR) == 0 ? 0 : -1;
}

int stream_shutdown_rd(stream_t* stream) {
    if (atomic_load(&stream->closed)) {
        return -1;
    }

    if (shutdown(stream->fd, PLATFORM_SHUT_RD) != 0) {
        return -1;
    }
    atomic_store(&stream->rd_shutdown, true);
    return 0;
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

    platform_sock_t fd = platform_socket_listen(
        host,
        port_str,
        SOCK_STREAM,
        true,
        enable_mss_clamp);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge(
            "<stream> listen failed host=%s port=%s",
            host ? host : "*",
            port_str);
        return NULL;
    }

    listener_t* listener = listener_from_fd(fd);
    if (!listener) {
        platform_socket_close(fd);
        return NULL;
    }
    return listener;
}

listener_t* listener_listen_unix(const char* path) {
    if (!path) {
        return NULL;
    }

    platform_sock_t fd = platform_socket_listen_unix(path, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("<stream> listen unix failed path=%s", path);
        return NULL;
    }

    listener_t* listener = listener_from_fd(fd);
    if (!listener) {
        platform_socket_close(fd);
        remove(path);
        return NULL;
    }
    return listener;
}

stream_t* listener_accept(listener_t* listener) {
    return _listener_accept(listener, false);
}

stream_t* listener_accept_unix(listener_t* listener) {
    return _listener_accept(listener, true);
}

void listener_close(listener_t* listener) {
    if (!atomic_exchange(&listener->closed, true)) {
        iowait_close(listener->waiter);
    }
}

void listener_destroy(listener_t* listener) {
    if (!listener) {
        return;
    }
    /* Detach iowait before closing the fd so a reused fd has no stale poller state. */
    listener_close(listener);
    if (listener->waiter) {
        iowait_destroy(listener->waiter);
    }
    if (listener->fd != PLATFORM_SO_ERROR_INVALID_SOCKET) {
        platform_socket_close(listener->fd);
    }
    free(listener);
}

int listener_addr(
    listener_t* listener,
    char*       host,
    size_t      host_len,
    uint16_t*   port) {
    if (atomic_load(&listener->closed)) {
        return -1;
    }

    addr_t    addr = {0};
    socklen_t len = sizeof(addr.storage);
    if (getsockname(listener->fd, (struct sockaddr*)&addr.storage, &len) != 0) {
        return -1;
    }
    return addr_ntop(&addr, host, host_len, port);
}
