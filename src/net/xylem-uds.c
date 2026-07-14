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

#include "net/stream.h"
#include "runtime/precond.h"
#include "runtime/runtime.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UDS_MAX_PATH 104

struct xylem_uds_conn_s {
    stream_t*    stream;
    _Atomic bool closed;
};

struct xylem_uds_listener_s {
    listener_t*  listener;
    char         path[UDS_MAX_PATH];
    _Atomic bool closed;
};

static xylem_uds_conn_t* _uds_conn_create(stream_t* stream) {
    xylem_uds_conn_t* uds
        = (xylem_uds_conn_t*)calloc(1, sizeof(xylem_uds_conn_t));
    if (!uds) {
        stream_destroy(stream);
        return NULL;
    }

    uds->stream = stream;
    atomic_init(&uds->closed, false);
    return uds;
}

static xylem_uds_listener_t* _uds_listener_create(
    listener_t* listener,
    const char* path) {
    xylem_uds_listener_t* uds_listener = (xylem_uds_listener_t*)calloc(
        1, sizeof(xylem_uds_listener_t));
    if (!uds_listener) {
        listener_destroy(listener);
        remove(path);
        return NULL;
    }

    uds_listener->listener = listener;
    snprintf(uds_listener->path, UDS_MAX_PATH, "%s", path);
    atomic_init(&uds_listener->closed, false);
    return uds_listener;
}

static void _uds_consume_io_credit(void) {
    if (runtime_consume_credit(RUNTIME_IO_CREDIT_COST)) {
        runtime_yield();
    }
}

xylem_uds_listener_t* xylem_uds_listen(const char* path) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_listen");

    if (!path || strlen(path) >= UDS_MAX_PATH) {
        xylem_loge("<uds> listen bad path len_max=%d", UDS_MAX_PATH - 1);
        return NULL;
    }

    listener_t* listener = listener_listen_unix(path);
    if (!listener) {
        return NULL;
    }
    return _uds_listener_create(listener, path);
}

xylem_uds_conn_t* xylem_uds_accept(xylem_uds_listener_t* listener) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_accept");

    xylem_uds_conn_t* conn = NULL;
    if (!atomic_load(&listener->closed)) {
        stream_t* stream = listener_accept_unix(listener->listener);
        if (stream) {
            conn = _uds_conn_create(stream);
        }
    }

    return conn;
}

void xylem_uds_close_listener(xylem_uds_listener_t* listener) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_close_listener");

    if (!atomic_exchange(&listener->closed, true)) {
        listener_close(listener->listener);
    }
}

void xylem_uds_destroy_listener(xylem_uds_listener_t* listener) {
    if (!listener) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_destroy_listener");

    xylem_uds_close_listener(listener);
    listener_destroy(listener->listener);
    if (listener->path[0] != '\0') {
        remove(listener->path);
    }
    free(listener);
}

xylem_uds_conn_t* xylem_uds_dial(
    const char* path,
    uint64_t    connect_timeout_ms) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_dial");

    if (!path || strlen(path) >= UDS_MAX_PATH) {
        xylem_loge("<uds> dial bad path len_max=%d", UDS_MAX_PATH - 1);
        return NULL;
    }

    stream_t* stream = stream_dial_unix(path, connect_timeout_ms);
    if (!stream) {
        return NULL;
    }
    return _uds_conn_create(stream);
}

void xylem_uds_set_read_deadline(
    xylem_uds_conn_t* uds,
    uint64_t          deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_set_read_deadline");

    if (!atomic_load(&uds->closed)) {
        stream_set_read_deadline(uds->stream, deadline_ms);
    }
}

void xylem_uds_set_write_deadline(
    xylem_uds_conn_t* uds,
    uint64_t          deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_set_write_deadline");

    if (!atomic_load(&uds->closed)) {
        stream_set_write_deadline(uds->stream, deadline_ms);
    }
}

int xylem_uds_read(xylem_uds_conn_t* uds, void* buf, int len) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_read");

    if (!buf || len <= 0) {
        return -1;
    }

    for (;;) {
        if (atomic_load(&uds->closed)) {
            return -1;
        }

        int n = stream_read(uds->stream, buf, len);
        if (n == 0) {
            return 0;
        }
        if (n > 0) {
            _uds_consume_io_credit();
            return n;
        }
        if (n != STREAM_IO_AGAIN
            || stream_wait_read(uds->stream) != IOWAIT_READY) {
            return -1;
        }
    }
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

    const char* ptr = (const char*)data;
    int         rem = len;

    while (rem > 0) {
        if (atomic_load(&uds->closed)) {
            return -1;
        }

        int n = stream_write(uds->stream, ptr, rem);
        if (n > 0) {
            ptr += n;
            rem -= n;
            _uds_consume_io_credit();
            continue;
        }
        if (n != STREAM_IO_AGAIN
            || stream_wait_write(uds->stream) != IOWAIT_READY) {
            return -1;
        }
    }

    return atomic_load(&uds->closed) ? -1 : 0;
}

void xylem_uds_close(xylem_uds_conn_t* uds) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_close");

    if (!atomic_exchange(&uds->closed, true)) {
        stream_close(uds->stream);
    }
}

void xylem_uds_destroy(xylem_uds_conn_t* uds) {
    if (!uds) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_destroy");

    xylem_uds_close(uds);
    stream_destroy(uds->stream);
    free(uds);
}

int xylem_uds_shutdown_wr(xylem_uds_conn_t* uds) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_shutdown_wr");

    int ret = -1;
    if (!atomic_load(&uds->closed)) {
        ret = stream_shutdown_wr(uds->stream);
    }
    return ret;
}

int xylem_uds_shutdown_rd(xylem_uds_conn_t* uds) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_shutdown_rd");

    int ret = -1;
    if (!atomic_load(&uds->closed)) {
        ret = stream_shutdown_rd(uds->stream);
    }
    return ret;
}
