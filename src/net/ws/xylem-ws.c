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

#include "xylem/net/xylem-ws.h"

#include "xylem/sync/xylem-mutex.h"

#include "net/http/http-utils.h"
#include "runtime/precond.h"
#include "ws-tcp.h"
#include "ws-tls.h"
#include "ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct xylem_ws_listener_s {
    void*          handle;
    ws_accept_fn_t accept;
    void (*close)(void*);
    void (*destroy)(void*);
    ws_listener_addr_fn_t addr;
    xylem_ws_opts_t       opts;
    xylem_mutex_t*        lock;
    ws_transport_t        pending;
    _Atomic bool          accepting;
    _Atomic bool          closing;
};

static int _ws_parse_url(
    const char* url,
    bool*       secure,
    char*       host,
    size_t      host_cap,
    uint16_t*   port,
    char*       path,
    size_t      path_cap) {
    if (!url) {
        return -1;
    }
    const char* rest;
    if (!strncmp(url, "wss://", 6)) {
        *secure = true;
        rest    = url + 6;
    } else if (!strncmp(url, "ws://", 5)) {
        *secure = false;
        rest    = url + 5;
    } else {
        return -1;
    }
    char converted[4096];
    int  n = snprintf(
        converted,
        sizeof(converted),
        "%s://%s",
        *secure ? "https" : "http",
        rest);
    http_url_t parsed;
    if (n < 0 || n >= sizeof(converted) || http_url_parse(converted, &parsed) ||
        strlen(parsed.host) >= host_cap || strlen(parsed.path) >= path_cap) {
        return -1;
    }
    snprintf(host, host_cap, "%s", parsed.host);
    snprintf(path, path_cap, "%s", parsed.path);
    *port = parsed.port;
    return 0;
}

xylem_ws_conn_t* xylem_ws_dial(const char* url, const xylem_ws_opts_t* opts) {
    char     host[256], path[1024];
    uint16_t port;
    bool     is_tls;
    if (_ws_parse_url(
            url,
            &is_tls,
            host,
            sizeof(host),
            &port,
            path,
            sizeof(path)) != 0) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_dial");

    /**
     * Dispatch on scheme to the matching dial factory: wss -> ws_tls_dial
     * (ws-tls.c, NULL stub when TLS is off), ws -> ws_tcp_dial (ws-tcp.c).
     */
    if (is_tls) {
        return ws_tls_dial(host, port, path, opts ? opts->tls : NULL, opts);
    }
    return ws_tcp_dial(host, port, path, opts);
}

xylem_ws_listener_t* ws_listener_create(
    void*          handle,
    ws_accept_fn_t accept,
    void (*close)(void*),
    void (*destroy)(void*),
    ws_listener_addr_fn_t  addr,
    const xylem_ws_opts_t* opts) {
    xylem_ws_listener_t* l =
        (xylem_ws_listener_t*)calloc(1, sizeof(xylem_ws_listener_t));
    if (!l) {
        destroy(handle);
        return NULL;
    }
    l->handle  = handle;
    l->accept  = accept;
    l->close   = close;
    l->destroy = destroy;
    l->addr    = addr;
    if (opts) {
        l->opts = *opts;
    }
    l->opts.tls = NULL;
    l->lock     = xylem_mutex_create();
    if (!l->lock) {
        destroy(handle);
        free(l);
        return NULL;
    }
    atomic_init(&l->accepting, false);
    atomic_init(&l->closing, false);
    return l;
}

xylem_ws_listener_t*
xylem_ws_listen(const char* host, uint16_t port, const xylem_ws_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_listen");
    return opts && opts->tls ? ws_tls_listen(host, port, opts)
                             : ws_tcp_listen(host, port, opts);
}

xylem_ws_conn_t* xylem_ws_accept(xylem_ws_listener_t* l) {
    if (!l) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_accept");
    if (atomic_exchange(&l->accepting, true)) {
        xylem_loge("<ws> concurrent accept");
        abort();
    }
    xylem_ws_conn_t* conn = NULL;
    while (!atomic_load(&l->closing)) {
        ws_transport_t t = l->accept(l->handle);
        if (!t.conn) {
            break;
        }
        xylem_mutex_lock(l->lock);
        bool closing = atomic_load(&l->closing);
        if (!closing) {
            l->pending = t;
        }
        xylem_mutex_unlock(l->lock);
        if (closing) {
            t.destroy(t.conn);
            break;
        }
        conn = ws_accept_impl(t, &l->opts);
        xylem_mutex_lock(l->lock);
        l->pending = (ws_transport_t){0};
        closing    = atomic_load(&l->closing);
        xylem_mutex_unlock(l->lock);
        if (conn) {
            if (closing) {
                ws_destroy(conn);
                conn = NULL;
            }
            break;
        }
        t.destroy(t.conn);
    }
    atomic_store(&l->accepting, false);
    return conn;
}

void xylem_ws_close_listener(xylem_ws_listener_t* l) {
    if (!l) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_close_listener");
    if (atomic_exchange(&l->closing, true)) {
        return;
    }
    l->close(l->handle);
    xylem_mutex_lock(l->lock);
    if (l->pending.conn) {
        /* The accept owner releases storage after cancellation returns. */
        l->pending.close(l->pending.conn);
    }
    xylem_mutex_unlock(l->lock);
}

void xylem_ws_destroy_listener(xylem_ws_listener_t* l) {
    if (!l) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_destroy_listener");
    xylem_ws_close_listener(l);
    l->destroy(l->handle);
    xylem_mutex_destroy(l->lock);
    free(l);
}

int xylem_ws_listener_addr(
    xylem_ws_listener_t* l,
    char*                host,
    size_t               host_len,
    uint16_t*            port) {
    if (!l) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("ws", "xylem_ws_listener_addr");
    return l->addr(l->handle, host, host_len, port);
}

void xylem_ws_set_read_deadline(xylem_ws_conn_t* conn, uint64_t deadline_ms) {
    ws_set_deadline(conn, deadline_ms, false);
}

void xylem_ws_set_write_deadline(xylem_ws_conn_t* conn, uint64_t deadline_ms) {
    ws_set_deadline(conn, deadline_ms, true);
}

int xylem_ws_remote_addr(
    xylem_ws_conn_t* conn,
    char*            host,
    size_t           host_len,
    uint16_t*        port) {
    return ws_addr(conn, host, host_len, port, true);
}

int xylem_ws_local_addr(
    xylem_ws_conn_t* conn,
    char*            host,
    size_t           host_len,
    uint16_t*        port) {
    return ws_addr(conn, host, host_len, port, false);
}

void xylem_ws_destroy(xylem_ws_conn_t* conn) {
    ws_destroy(conn);
}

int xylem_ws_send(
    xylem_ws_conn_t*  conn,
    const void*       data,
    int               len,
    xylem_ws_opcode_t type) {
    return ws_send(conn, data, len, type);
}

int xylem_ws_ping(xylem_ws_conn_t* conn, const void* data, size_t len) {
    return ws_ping(conn, data, len);
}

int xylem_ws_recv(
    xylem_ws_conn_t*   conn,
    void*              buf,
    int                len,
    xylem_ws_opcode_t* type) {
    return ws_recv(conn, buf, len, type);
}

int xylem_ws_close(
    xylem_ws_conn_t* conn,
    uint16_t         code,
    const char*      reason,
    size_t           reason_len) {
    return ws_close(conn, code, reason, reason_len);
}

void* xylem_ws_get_userdata(xylem_ws_conn_t* conn) {
    return ws_get_userdata(conn);
}

void xylem_ws_set_userdata(xylem_ws_conn_t* conn, void* ud) {
    ws_set_userdata(conn, ud);
}
