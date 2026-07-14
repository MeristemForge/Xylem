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

#include "net/stream.h"
#include "runtime/precond.h"

#include <stdatomic.h>
#include <stdlib.h>

struct xylem_tcp_conn_s {
    stream_t*       stream;
    _Atomic int32_t refcnt;
    _Atomic bool    closed;
};

struct xylem_tcp_listener_s {
    listener_t*     listener;
    _Atomic int32_t refcnt;
    _Atomic bool    closed;
};

static void _tcp_conn_ref(xylem_tcp_conn_t* tcp) {
    atomic_fetch_add(&tcp->refcnt, 1);
}

static void _tcp_conn_unref(xylem_tcp_conn_t* tcp) {
    if (atomic_fetch_sub(&tcp->refcnt, 1) != 1) {
        return;
    }
    stream_destroy(tcp->stream);
    free(tcp);
}

static xylem_tcp_conn_t* _tcp_conn_create(stream_t* stream) {
    xylem_tcp_conn_t* tcp
        = (xylem_tcp_conn_t*)calloc(1, sizeof(xylem_tcp_conn_t));
    if (!tcp) {
        stream_destroy(stream);
        return NULL;
    }

    tcp->stream = stream;
    atomic_init(&tcp->refcnt, 1);
    atomic_init(&tcp->closed, false);
    return tcp;
}

static void _tcp_listener_ref(xylem_tcp_listener_t* listener) {
    atomic_fetch_add(&listener->refcnt, 1);
}

static void _tcp_listener_unref(xylem_tcp_listener_t* listener) {
    if (atomic_fetch_sub(&listener->refcnt, 1) != 1) {
        return;
    }
    listener_destroy(listener->listener);
    free(listener);
}

static xylem_tcp_listener_t* _tcp_listener_create(listener_t* listener) {
    xylem_tcp_listener_t* tcp_listener
        = (xylem_tcp_listener_t*)calloc(1, sizeof(xylem_tcp_listener_t));
    if (!tcp_listener) {
        listener_destroy(listener);
        return NULL;
    }

    tcp_listener->listener = listener;
    atomic_init(&tcp_listener->refcnt, 1);
    atomic_init(&tcp_listener->closed, false);
    return tcp_listener;
}

xylem_tcp_listener_t* xylem_tcp_listen(
    const char*       host,
    uint16_t          port,
    xylem_tcp_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("tcp", "xylem_tcp_listen");

    bool enable_mss_clamp = opts && opts->enable_mss_clamp;
    listener_t* listener
        = listener_listen(host, port, enable_mss_clamp);
    if (!listener) {
        return NULL;
    }

    return _tcp_listener_create(listener);
}

xylem_tcp_conn_t* xylem_tcp_accept(xylem_tcp_listener_t* listener) {
    RUNTIME_REQUIRE_COROUTINE("tcp", "xylem_tcp_accept");

    _tcp_listener_ref(listener);

    xylem_tcp_conn_t* conn = NULL;
    if (!atomic_load(&listener->closed)) {
        stream_t* stream = listener_accept(listener->listener);
        if (stream) {
            conn = _tcp_conn_create(stream);
        }
    }

    _tcp_listener_unref(listener);
    return conn;
}

void xylem_tcp_close_listener(xylem_tcp_listener_t* listener) {
    RUNTIME_REQUIRE_COROUTINE("tcp", "xylem_tcp_close_listener");

    if (atomic_exchange(&listener->closed, true)) {
        return;
    }

    listener_close(listener->listener);
    _tcp_listener_unref(listener);
}

xylem_tcp_conn_t* xylem_tcp_dial(
    const char*       host,
    uint16_t          port,
    xylem_tcp_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("tcp", "xylem_tcp_dial");

    uint64_t connect_timeout_ms = opts ? opts->connect_timeout_ms : 0;
    bool enable_mss_clamp = opts && opts->enable_mss_clamp;
    stream_t* stream
        = stream_dial(host, port, connect_timeout_ms, enable_mss_clamp);
    if (!stream) {
        return NULL;
    }

    return _tcp_conn_create(stream);
}

void xylem_tcp_set_read_deadline(
    xylem_tcp_conn_t* tcp,
    uint64_t          deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("tcp", "xylem_tcp_set_read_deadline");

    _tcp_conn_ref(tcp);
    if (!atomic_load(&tcp->closed)) {
        stream_set_read_deadline(tcp->stream, deadline_ms);
    }
    _tcp_conn_unref(tcp);
}

void xylem_tcp_set_write_deadline(
    xylem_tcp_conn_t* tcp,
    uint64_t          deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("tcp", "xylem_tcp_set_write_deadline");

    _tcp_conn_ref(tcp);
    if (!atomic_load(&tcp->closed)) {
        stream_set_write_deadline(tcp->stream, deadline_ms);
    }
    _tcp_conn_unref(tcp);
}

int xylem_tcp_read(xylem_tcp_conn_t* tcp, void* buf, int len) {
    RUNTIME_REQUIRE_COROUTINE("tcp", "xylem_tcp_read");

    if (!buf || len <= 0) {
        return -1;
    }

    _tcp_conn_ref(tcp);
    int ret = -1;
    if (!atomic_load(&tcp->closed)) {
        ret = stream_read(tcp->stream, buf, len);
    }
    _tcp_conn_unref(tcp);
    return ret;
}

int xylem_tcp_write(xylem_tcp_conn_t* tcp, const void* data, int len) {
    RUNTIME_REQUIRE_COROUTINE("tcp", "xylem_tcp_write");

    if (len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }

    _tcp_conn_ref(tcp);
    int ret = -1;
    if (!atomic_load(&tcp->closed)) {
        ret = stream_write(tcp->stream, data, len);
    }
    _tcp_conn_unref(tcp);
    return ret;
}

void xylem_tcp_close(xylem_tcp_conn_t* tcp) {
    RUNTIME_REQUIRE_COROUTINE("tcp", "xylem_tcp_close");

    if (atomic_exchange(&tcp->closed, true)) {
        return;
    }
    stream_close(tcp->stream);
    _tcp_conn_unref(tcp);
}

int xylem_tcp_remote_addr(
    xylem_tcp_conn_t* tcp,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    RUNTIME_REQUIRE_COROUTINE("tcp", "xylem_tcp_remote_addr");

    _tcp_conn_ref(tcp);
    int ret = -1;
    if (!atomic_load(&tcp->closed)) {
        ret = stream_remote_addr(tcp->stream, host, host_len, port);
    }
    _tcp_conn_unref(tcp);
    return ret;
}

int xylem_tcp_local_addr(
    xylem_tcp_conn_t* tcp,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    RUNTIME_REQUIRE_COROUTINE("tcp", "xylem_tcp_local_addr");

    _tcp_conn_ref(tcp);
    int ret = -1;
    if (!atomic_load(&tcp->closed)) {
        ret = stream_local_addr(tcp->stream, host, host_len, port);
    }
    _tcp_conn_unref(tcp);
    return ret;
}

int xylem_tcp_listener_addr(
    xylem_tcp_listener_t* listener,
    char*                 host,
    size_t                host_len,
    uint16_t*             port) {
    RUNTIME_REQUIRE_COROUTINE("tcp", "xylem_tcp_listener_addr");

    _tcp_listener_ref(listener);
    int ret = -1;
    if (!atomic_load(&listener->closed)) {
        ret = listener_addr(listener->listener, host, host_len, port);
    }
    _tcp_listener_unref(listener);
    return ret;
}

int xylem_tcp_shutdown_wr(xylem_tcp_conn_t* tcp) {
    RUNTIME_REQUIRE_COROUTINE("tcp", "xylem_tcp_shutdown_wr");

    _tcp_conn_ref(tcp);
    int ret = -1;
    if (!atomic_load(&tcp->closed)) {
        ret = stream_shutdown_wr(tcp->stream);
    }
    _tcp_conn_unref(tcp);
    return ret;
}

int xylem_tcp_shutdown_rd(xylem_tcp_conn_t* tcp) {
    RUNTIME_REQUIRE_COROUTINE("tcp", "xylem_tcp_shutdown_rd");

    _tcp_conn_ref(tcp);
    int ret = -1;
    if (!atomic_load(&tcp->closed)) {
        ret = stream_shutdown_rd(tcp->stream);
    }
    _tcp_conn_unref(tcp);
    return ret;
}
