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
    listener_t*     listener;
    char            path[UDS_MAX_PATH];
    _Atomic int32_t refcnt;
    _Atomic bool    closed;
};

static void _uds_conn_ref(xylem_uds_conn_t* uds) {
    atomic_fetch_add(&uds->refcnt, 1);
}

static void _uds_conn_unref(xylem_uds_conn_t* uds) {
    if (atomic_fetch_sub(&uds->refcnt, 1) != 1) {
        return;
    }
    stream_release(uds->stream);
    free(uds);
}

static xylem_uds_conn_t* _uds_conn_create(stream_t* stream) {
    xylem_uds_conn_t* uds
        = (xylem_uds_conn_t*)calloc(1, sizeof(xylem_uds_conn_t));
    if (!uds) {
        stream_release(stream);
        return NULL;
    }

    uds->stream = stream;
    atomic_init(&uds->refcnt, 1);
    atomic_init(&uds->closed, false);
    return uds;
}

static void _uds_listener_ref(xylem_uds_listener_t* listener) {
    atomic_fetch_add(&listener->refcnt, 1);
}

static void _uds_listener_unref(xylem_uds_listener_t* listener) {
    if (atomic_fetch_sub(&listener->refcnt, 1) != 1) {
        return;
    }
    listener_release(listener->listener);
    if (listener->path[0] != '\0') {
        remove(listener->path);
    }
    free(listener);
}

static xylem_uds_listener_t* _uds_listener_create(
    listener_t* listener,
    const char* path) {
    xylem_uds_listener_t* uds_listener = (xylem_uds_listener_t*)calloc(
        1, sizeof(xylem_uds_listener_t));
    if (!uds_listener) {
        listener_release(listener);
        remove(path);
        return NULL;
    }

    uds_listener->listener = listener;
    snprintf(uds_listener->path, UDS_MAX_PATH, "%s", path);
    atomic_init(&uds_listener->refcnt, 1);
    atomic_init(&uds_listener->closed, false);
    return uds_listener;
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

    _uds_listener_ref(listener);

    xylem_uds_conn_t* conn = NULL;
    if (!atomic_load(&listener->closed)) {
        stream_t* stream = listener_accept_unix(listener->listener);
        if (stream) {
            conn = _uds_conn_create(stream);
        }
    }

    _uds_listener_unref(listener);
    return conn;
}

void xylem_uds_close_listener(xylem_uds_listener_t* listener) {
    RUNTIME_REQUIRE_COROUTINE("uds", "xylem_uds_close_listener");

    if (atomic_exchange(&listener->closed, true)) {
        return;
    }

    listener_interrupt(listener->listener);
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

    _uds_conn_ref(uds);
    if (!atomic_load(&uds->closed)) {
        stream_set_read_deadline(uds->stream, deadline_ms);
    }
    _uds_conn_unref(uds);
}

void xylem_uds_set_write_deadline(
    xylem_uds_conn_t* uds,
    uint64_t          deadline_ms) {
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
