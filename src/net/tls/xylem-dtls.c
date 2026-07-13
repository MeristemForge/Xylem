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
 * Thin opaque-handle shim over the internal DTLS engine in tls.c. Each
 * public handle wraps the engine's concrete struct as its sole, offset-0
 * member, so C first-member address equivalence permits direct casts
 * between the public and internal handles.
 */

#include "xylem/net/xylem-dtls.h"

#include "tls.h"

#include "runtime/precond.h"

#include <stddef.h>

struct xylem_dtls_ctx_s {
    tls_ctx_t internal;
};

struct xylem_dtls_conn_s {
    dtls_conn_t internal;
};

struct xylem_dtls_listener_s {
    dtls_listener_t internal;
};

_Static_assert(offsetof(struct xylem_dtls_ctx_s, internal) == 0,
               "tls_ctx_t must remain the first member of xylem_dtls_ctx_s");
_Static_assert(offsetof(struct xylem_dtls_conn_s, internal) == 0,
               "dtls_conn_t must remain the first member of "
               "xylem_dtls_conn_s");
_Static_assert(offsetof(struct xylem_dtls_listener_s, internal) == 0,
               "dtls_listener_t must remain the first member of "
               "xylem_dtls_listener_s");

xylem_dtls_ctx_t* xylem_dtls_ctx_create(void) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_create");

    return (xylem_dtls_ctx_t*)dtls_ctx_create();
}

void xylem_dtls_ctx_destroy(xylem_dtls_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_destroy");

    tls_ctx_destroy(&ctx->internal);
}

int xylem_dtls_ctx_set_keylog(xylem_dtls_ctx_t* ctx, const char* path) {
    if (!ctx) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_set_keylog");

    return tls_ctx_set_keylog(&ctx->internal, path);
}

int xylem_dtls_ctx_load_cert(
    xylem_dtls_ctx_t* ctx,
    const char*       hostname,
    const char*       cert,
    const char*       key) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_load_cert");

    return tls_ctx_load_cert(&ctx->internal, hostname, cert, key);
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

    return tls_ctx_load_cert_mem(&ctx->internal, hostname, cert_pem, cert_len,
                                 key_pem, key_len);
}

int xylem_dtls_ctx_load_ca(xylem_dtls_ctx_t* ctx, const char* ca_file) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_load_ca");

    return tls_ctx_load_ca(&ctx->internal, ca_file);
}

int xylem_dtls_ctx_load_system_ca(
    xylem_dtls_ctx_t* ctx,
    const char*       fallback_ca_file) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_load_system_ca");

    return tls_ctx_load_system_ca(&ctx->internal, fallback_ca_file);
}

void xylem_dtls_ctx_verify_server(xylem_dtls_ctx_t* ctx, bool enable) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_verify_server");

    tls_ctx_verify_server(&ctx->internal, enable);
}

void xylem_dtls_ctx_verify_client(xylem_dtls_ctx_t* ctx, bool enable) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_verify_client");

    tls_ctx_verify_client(&ctx->internal, enable);
}

int xylem_dtls_ctx_set_alpn(
    xylem_dtls_ctx_t* ctx,
    const char**      protocols,
    size_t            count) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_ctx_set_alpn");

    return tls_ctx_set_alpn(&ctx->internal, protocols, count);
}

xylem_dtls_conn_t* xylem_dtls_dial(
    const char*        host,
    uint16_t           port,
    xylem_dtls_ctx_t*  ctx,
    xylem_dtls_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_dial");

    return (xylem_dtls_conn_t*)dtls_dial(
        host, port, &ctx->internal, opts);
}

xylem_dtls_listener_t* xylem_dtls_listen(
    const char*        host,
    uint16_t           port,
    xylem_dtls_ctx_t*  ctx,
    xylem_dtls_opts_t* opts) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_listen");

    return (xylem_dtls_listener_t*)dtls_listen(
        host, port, &ctx->internal, opts);
}

xylem_dtls_conn_t* xylem_dtls_accept(xylem_dtls_listener_t* ln) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_accept");

    return (xylem_dtls_conn_t*)dtls_accept(&ln->internal);
}

int xylem_dtls_read(xylem_dtls_conn_t* dtls, void* buf, int len) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_read");

    if (!buf || len <= 0) {
        return -1;
    }
    return dtls_read(&dtls->internal, buf, len);
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
    return dtls_write(&dtls->internal, data, len);
}

void xylem_dtls_close(xylem_dtls_conn_t* dtls) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_close");

    dtls_close(&dtls->internal);
}

void xylem_dtls_close_listener(xylem_dtls_listener_t* ln) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_close_listener");

    dtls_close_listener(&ln->internal);
}

void xylem_dtls_set_read_deadline(
    xylem_dtls_conn_t* dtls,
    uint64_t           deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_set_read_deadline");

    dtls_set_read_deadline(&dtls->internal, deadline_ms);
}

void xylem_dtls_set_write_deadline(
    xylem_dtls_conn_t* dtls,
    uint64_t           deadline_ms) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_set_write_deadline");

    dtls_set_write_deadline(&dtls->internal, deadline_ms);
}

const char* xylem_dtls_get_alpn(xylem_dtls_conn_t* dtls) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_get_alpn");

    return dtls_get_alpn(&dtls->internal);
}

int xylem_dtls_remote_addr(
    xylem_dtls_conn_t* dtls,
    char*              host,
    size_t             host_len,
    uint16_t*          port) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_remote_addr");

    return dtls_remote_addr(&dtls->internal, host, host_len, port);
}

int xylem_dtls_local_addr(
    xylem_dtls_conn_t* dtls,
    char*              host,
    size_t             host_len,
    uint16_t*          port) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_local_addr");

    return dtls_local_addr(&dtls->internal, host, host_len, port);
}

int xylem_dtls_listener_addr(
    xylem_dtls_listener_t* ln,
    char*                  host,
    size_t                 host_len,
    uint16_t*              port) {
    RUNTIME_REQUIRE_COROUTINE("dtls", "xylem_dtls_listener_addr");

    return dtls_listener_addr(&ln->internal, host, host_len, port);
}
