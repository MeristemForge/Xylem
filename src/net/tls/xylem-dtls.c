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

/**
 * DTLS public API surface.
 *
 * Thin opaque-handle shim over the internal DTLS engine in tls.c. Every
 * public opaque type aliases its matching internal engine type.
 */

#include "xylem/net/xylem-dtls.h"

#include "tls.h"

#include "runtime/precond.h"

xylem_dtls_ctx_t* xylem_dtls_ctx_create(void) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_create");

    return dtls_ctx_create();
}

void xylem_dtls_ctx_destroy(xylem_dtls_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_destroy");

    dtls_ctx_destroy(ctx);
}

int xylem_dtls_ctx_set_keylog(xylem_dtls_ctx_t* ctx, const char* path) {
    if (!ctx) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_set_keylog");

    return dtls_ctx_set_keylog(ctx, path);
}

int xylem_dtls_ctx_load_cert(
    xylem_dtls_ctx_t* ctx,
    const char*       hostname,
    const char*       cert,
    const char*       key) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_load_cert");

    return dtls_ctx_load_cert(ctx, hostname, cert, key);
}

int xylem_dtls_ctx_load_cert_mem(
    xylem_dtls_ctx_t* ctx,
    const char*       hostname,
    const void*       cert_pem,
    size_t            cert_len,
    const void*       key_pem,
    size_t            key_len) {
    if (!cert_pem || cert_len == 0 || !key_pem || key_len == 0) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_load_cert_mem");

    return dtls_ctx_load_cert_mem(ctx, hostname, cert_pem, cert_len, key_pem,
                                  key_len);
}

int xylem_dtls_ctx_load_ca(xylem_dtls_ctx_t* ctx, const char* ca_file) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_load_ca");

    return dtls_ctx_load_ca(ctx, ca_file);
}

int xylem_dtls_ctx_load_system_ca(
    xylem_dtls_ctx_t* ctx,
    const char*       fallback_ca_file) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_load_system_ca");

    return dtls_ctx_load_system_ca(ctx, fallback_ca_file);
}

void xylem_dtls_ctx_verify_server(xylem_dtls_ctx_t* ctx, bool enable) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_verify_server");

    dtls_ctx_verify_server(ctx, enable);
}

void xylem_dtls_ctx_verify_client(xylem_dtls_ctx_t* ctx, bool enable) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_verify_client");

    dtls_ctx_verify_client(ctx, enable);
}

int xylem_dtls_ctx_set_alpn(
    xylem_dtls_ctx_t* ctx,
    const char**      protocols,
    size_t            count) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_set_alpn");

    return dtls_ctx_set_alpn(ctx, protocols, count);
}

xylem_dtls_conn_t* xylem_dtls_dial(
    const char*        host,
    uint16_t           port,
    xylem_dtls_ctx_t*  ctx,
    xylem_dtls_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_dial");

    return dtls_dial(host, port, ctx, opts);
}

xylem_dtls_listener_t* xylem_dtls_listen(
    const char*        host,
    uint16_t           port,
    xylem_dtls_ctx_t*  ctx,
    xylem_dtls_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_listen");

    return dtls_listen(host, port, ctx, opts);
}

xylem_dtls_conn_t* xylem_dtls_accept(xylem_dtls_listener_t* ln) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_accept");

    return dtls_accept(ln);
}

int xylem_dtls_read(xylem_dtls_conn_t* dtls, void* buf, int len) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_read");

    if (!buf || len <= 0) {
        return -1;
    }
    return dtls_read(dtls, buf, len);
}

int xylem_dtls_write(
    xylem_dtls_conn_t* dtls,
    const void*        data,
    int                len) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_write");

    if (len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }
    return dtls_write(dtls, data, len);
}

void xylem_dtls_close(xylem_dtls_conn_t* dtls) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_close");

    dtls_close(dtls);
}

void xylem_dtls_destroy(xylem_dtls_conn_t* dtls) {
    if (!dtls) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_destroy");

    dtls_destroy(dtls);
}

void xylem_dtls_close_listener(xylem_dtls_listener_t* ln) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_close_listener");

    dtls_close_listener(ln);
}

void xylem_dtls_destroy_listener(xylem_dtls_listener_t* ln) {
    if (!ln) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_destroy_listener");

    dtls_destroy_listener(ln);
}

void xylem_dtls_set_read_deadline(
    xylem_dtls_conn_t* dtls,
    uint64_t           deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_set_read_deadline");

    dtls_set_read_deadline(dtls, deadline_ms);
}

void xylem_dtls_set_write_deadline(
    xylem_dtls_conn_t* dtls,
    uint64_t           deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_set_write_deadline");

    dtls_set_write_deadline(dtls, deadline_ms);
}

const char* xylem_dtls_get_alpn(xylem_dtls_conn_t* dtls) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_get_alpn");

    return dtls_get_alpn(dtls);
}

int xylem_dtls_remote_addr(
    xylem_dtls_conn_t* dtls,
    char*              host,
    size_t             host_len,
    uint16_t*          port) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_remote_addr");

    return dtls_remote_addr(dtls, host, host_len, port);
}

int xylem_dtls_local_addr(
    xylem_dtls_conn_t* dtls,
    char*              host,
    size_t             host_len,
    uint16_t*          port) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_local_addr");

    return dtls_local_addr(dtls, host, host_len, port);
}

int xylem_dtls_listener_addr(
    xylem_dtls_listener_t* ln,
    char*                  host,
    size_t                 host_len,
    uint16_t*              port) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_listener_addr");

    return dtls_listener_addr(ln, host, host_len, port);
}
