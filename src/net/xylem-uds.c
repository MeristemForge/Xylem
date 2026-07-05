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

#include "xylem/net/xylem-uds.h"

#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "net/stream.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"
#include "runtime/precond.h"
#include "runtime/runtime.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UDS_MAX_PATH 104

struct xylem_uds_conn_s {
    stream_t*       stream;
    _Atomic int32_t refcnt;
    _Atomic bool    closed;
};

struct xylem_uds_listener_s {
    iowait_t*       waiter;
    platform_sock_t fd;
    char            path[UDS_MAX_PATH];
    _Atomic int32_t refcnt;
    _Atomic bool    closed;
};

static void _uds_conn_ref(xylem_uds_conn_t* uds) {
    atomic_fetch_add(&uds->refcnt, 1);
}

static void _uds_conn_unref(xylem_uds_conn_t* uds) {
    if (atomic_fetch_sub(&uds->refcnt, 1)
        != 1) {
        return;
    }
    stream_release(uds->stream);
    free(uds);
}

static void _uds_listener_ref(xylem_uds_listener_t* ln) {
    atomic_fetch_add(&ln->refcnt, 1);
}

static void _uds_listener_unref(xylem_uds_listener_t* ln) {
    if (atomic_fetch_sub(&ln->refcnt, 1)
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

static xylem_uds_conn_t* _uds_conn_create(platform_sock_t fd) {
    xylem_uds_conn_t* uds
        = (xylem_uds_conn_t*)calloc(1, sizeof(xylem_uds_conn_t));
    if (!uds) {
        platform_socket_close(fd);
        return NULL;
    }

    uds->stream = stream_from_fd(fd);
    if (!uds->stream) {
        platform_socket_close(fd);
        free(uds);
        return NULL;
    }

    _uds_conn_ref(uds);
    return uds;
}

xylem_uds_listener_t* xylem_uds_listen(const char* path) {
    if (!path || strlen(path) >= UDS_MAX_PATH) {
        xylem_loge("<uds> listen bad path len_max=%d", UDS_MAX_PATH - 1);
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_listen");

    platform_sock_t fd = platform_socket_listen_unix(path, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("<uds> listen failed path=%s", path);
        return NULL;
    }

    xylem_uds_listener_t* listener = (xylem_uds_listener_t*)calloc(
        1, sizeof(xylem_uds_listener_t));
    if (!listener) {
        platform_socket_close(fd);
        return NULL;
    }

    listener->fd = fd;
    snprintf(listener->path, UDS_MAX_PATH, "%s", path);

    listener->waiter = iowait_create(fd);
    if (!listener->waiter) {
        platform_socket_close(fd);
        free(listener);
        return NULL;
    }

    _uds_listener_ref(listener);
    return listener;
}

xylem_uds_conn_t* xylem_uds_accept(xylem_uds_listener_t* listener) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_accept");

    _uds_listener_ref(listener);

    xylem_uds_conn_t* result = NULL;
    uint64_t          backoff_ms = 5;
    int               retries   = 0;

    for (;;) {
        if (atomic_load(&listener->closed)) {
            break;
        }

        platform_sock_t fd
            = platform_socket_accept_unix(listener->fd, true);
        if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
            int err = platform_socket_get_lasterror();
            if (err == PLATFORM_SO_ERROR_EAGAIN
                || err == PLATFORM_SO_ERROR_EWOULDBLOCK) {
                if (iowait_read(listener->waiter) != IOWAIT_READY) {
                    break;
                }
                continue;
            }

            xylem_loge("<uds> accept failed fd=%d err=%d (%s)",
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

        xylem_uds_conn_t* uds = _uds_conn_create(fd);
        if (!uds) {
            break;
        }

        result = uds;
        break;
    }

    _uds_listener_unref(listener);
    return result;
}

void xylem_uds_close_listener(xylem_uds_listener_t* listener) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_close_listener");

    if (atomic_exchange(&listener->closed, true)) {
        return;
    }

    iowait_close(listener->waiter);

    if (listener->path[0] != '\0') {
        remove(listener->path);
    }

    _uds_listener_unref(listener);
}

xylem_uds_conn_t* xylem_uds_dial(
    const char* path,
    uint64_t    connect_timeout_ms) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_dial");

    if (!path || strlen(path) >= UDS_MAX_PATH) {
        xylem_loge("<uds> dial bad path len_max=%d", UDS_MAX_PATH - 1);
        return NULL;
    }

    bool connected = false;
    platform_sock_t fd
        = platform_socket_dial_unix(path, &connected, true);
    if (fd == PLATFORM_SO_ERROR_INVALID_SOCKET) {
        xylem_loge("<uds> dial failed path=%s", path);
        return NULL;
    }

    xylem_uds_conn_t* uds = _uds_conn_create(fd);
    if (!uds) {
        return NULL;
    }

    if (!connected) {
        if (connect_timeout_ms > 0) {
            uint64_t deadline
                = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)
                  + connect_timeout_ms;
            stream_set_write_deadline(uds->stream, deadline);
        }
        iowait_result_t rc = stream_wait_write(uds->stream);
        stream_set_write_deadline(uds->stream, 0);

        if (rc != IOWAIT_READY) {
            xylem_loge("<uds> dial connect timeout path=%s", path);
            xylem_uds_close(uds);
            return NULL;
        }

        int32_t   err    = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(
            stream_fd(uds->stream), SOL_SOCKET, SO_ERROR, (char*)&err, &errlen);
        if (err != 0) {
            xylem_loge("<uds> dial connect failed fd=%d err=%d (%s)",
                       (int)stream_fd(uds->stream),
                       err,
                       platform_socket_tostring(err));
            xylem_uds_close(uds);
            return NULL;
        }
    }

    return uds;
}

void xylem_uds_set_read_deadline(
    xylem_uds_conn_t* uds, uint64_t deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_set_read_deadline");

    _uds_conn_ref(uds);
    if (!atomic_load(&uds->closed)) {
        stream_set_read_deadline(uds->stream, deadline_ms);
    }
    _uds_conn_unref(uds);
}

void xylem_uds_set_write_deadline(
    xylem_uds_conn_t* uds, uint64_t deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_set_write_deadline");

    _uds_conn_ref(uds);
    if (!atomic_load(&uds->closed)) {
        stream_set_write_deadline(uds->stream, deadline_ms);
    }
    _uds_conn_unref(uds);
}

int xylem_uds_read(xylem_uds_conn_t* uds, void* buf, int len) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_read");

    if (!buf || len <= 0) {
        return -1;
    }

    _uds_conn_ref(uds);
    int ret = -1;

    if (!atomic_load(&uds->closed)) {
        ret = stream_read(uds->stream, buf, len);
    }

    _uds_conn_unref(uds);
    return ret;
}

int xylem_uds_write(xylem_uds_conn_t* uds, const void* data, int len) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_write");

    if (len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }

    _uds_conn_ref(uds);
    int ret = -1;

    if (!atomic_load(&uds->closed)) {
        ret = stream_write(uds->stream, data, len);
    }

    _uds_conn_unref(uds);
    return ret;
}

void xylem_uds_close(xylem_uds_conn_t* uds) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_close");

    if (atomic_exchange(&uds->closed, true)) {
        return;
    }
    stream_interrupt(uds->stream);
    _uds_conn_unref(uds);
}

int xylem_uds_shutdown_wr(xylem_uds_conn_t* uds) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_shutdown_wr");

    _uds_conn_ref(uds);
    int ret = -1;
    if (!atomic_load(&uds->closed)) {
        ret = stream_shutdown_wr(uds->stream);
    }
    _uds_conn_unref(uds);
    return ret;
}

int xylem_uds_shutdown_rd(xylem_uds_conn_t* uds) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_shutdown_rd");

    _uds_conn_ref(uds);
    int ret = -1;
    if (!atomic_load(&uds->closed)) {
        ret = stream_shutdown_rd(uds->stream);
    }
    _uds_conn_unref(uds);
    return ret;
}
