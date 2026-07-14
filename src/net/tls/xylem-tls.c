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
 * TLS public API surface.
 *
 * Thin opaque-handle shim over the internal TLS engine (tls.c). Each
 * public handle wraps the engine's concrete struct as its sole, offset-0
 * member, so by C first-member address equivalence (6.7.2.1) a plain cast
 * converts between the two and every shim function just forwards to the
 * matching tls_* entry point. xylem_tls_opts_t is a transparent value
 * type the engine uses as-is, so it needs no wrapper.
 */

#include "xylem/net/xylem-tls.h"

#include "tls.h"

#include "runtime/precond.h"

#include <stddef.h>

struct xylem_tls_ctx_s {
    tls_ctx_t internal;
};

struct xylem_tls_conn_s {
    tls_conn_t internal;
};

struct xylem_tls_listener_s {
    tls_listener_t internal;
};

_Static_assert(offsetof(struct xylem_tls_ctx_s, internal) == 0,
               "tls_ctx_t must remain the first member of xylem_tls_ctx_s");
_Static_assert(offsetof(struct xylem_tls_conn_s, internal) == 0,
               "tls_conn_t must remain the first member of xylem_tls_conn_s");
_Static_assert(offsetof(struct xylem_tls_listener_s, internal) == 0,
               "tls_listener_t must remain the first member of "
               "xylem_tls_listener_s");

xylem_tls_ctx_t* xylem_tls_ctx_create(void) {
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_ctx_create");

    return (xylem_tls_ctx_t*)tls_ctx_create();
}

void xylem_tls_ctx_destroy(xylem_tls_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_ctx_destroy");

    tls_ctx_destroy(&ctx->internal);
}

int xylem_tls_ctx_set_keylog(xylem_tls_ctx_t* ctx, const char* path) {
    if (!ctx) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_ctx_set_keylog");

    return tls_ctx_set_keylog(&ctx->internal, path);
}

int xylem_tls_ctx_load_cert(
    xylem_tls_ctx_t* ctx,
    const char*      hostname,
    const char*      cert,
    const char*      key) {
    if (!ctx) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_ctx_load_cert");

    return tls_ctx_load_cert(&ctx->internal, hostname, cert, key);
}

int xylem_tls_ctx_load_cert_mem(
    xylem_tls_ctx_t* ctx,
    const char*      hostname,
    const void*      cert_pem,
    size_t           cert_len,
    const void*      key_pem,
    size_t           key_len) {
    if (!ctx) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_ctx_load_cert_mem");

    return tls_ctx_load_cert_mem(&ctx->internal, hostname, cert_pem, cert_len,
                                 key_pem, key_len);
}

int xylem_tls_ctx_load_ca(xylem_tls_ctx_t* ctx, const char* ca_file) {
    if (!ctx) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_ctx_load_ca");

    return tls_ctx_load_ca(&ctx->internal, ca_file);
}

int xylem_tls_ctx_load_system_ca(
    xylem_tls_ctx_t* ctx,
    const char*      fallback_ca_file) {
    if (!ctx) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_ctx_load_system_ca");

    return tls_ctx_load_system_ca(&ctx->internal, fallback_ca_file);
}

void xylem_tls_ctx_verify_server(xylem_tls_ctx_t* ctx, bool enable) {
    if (ctx) {
        RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_ctx_verify_server");

        tls_ctx_verify_server(&ctx->internal, enable);
    }
}

void xylem_tls_ctx_verify_client(xylem_tls_ctx_t* ctx, bool enable) {
    if (ctx) {
        RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_ctx_verify_client");

        tls_ctx_verify_client(&ctx->internal, enable);
    }
}

int xylem_tls_ctx_set_alpn(
    xylem_tls_ctx_t* ctx,
    const char**     protocols,
    size_t           count) {
    if (!ctx) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_ctx_set_alpn");

    return tls_ctx_set_alpn(&ctx->internal, protocols, count);
}

xylem_tls_conn_t* xylem_tls_dial(
    const char*       host,
    uint16_t          port,
    xylem_tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts) {
    if (!ctx) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_dial");

    return (xylem_tls_conn_t*)tls_dial(host, port, &ctx->internal, opts);
}

void xylem_tls_close(xylem_tls_conn_t* tls) {
    if (tls) {
        RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_close");

        tls_close(&tls->internal);
    }
}

void xylem_tls_destroy(xylem_tls_conn_t* tls) {
    if (!tls) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_destroy");

    tls_destroy(&tls->internal);
}

xylem_tls_listener_t* xylem_tls_listen(
    const char*       host,
    uint16_t          port,
    xylem_tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts) {
    if (!ctx) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_listen");

    return (xylem_tls_listener_t*)tls_listen(host, port, &ctx->internal, opts);
}

xylem_tls_conn_t* xylem_tls_accept(xylem_tls_listener_t* ln) {
    if (!ln) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_accept");

    return (xylem_tls_conn_t*)tls_accept(&ln->internal);
}

void xylem_tls_close_listener(xylem_tls_listener_t* ln) {
    if (ln) {
        RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_close_listener");

        tls_close_listener(&ln->internal);
    }
}

void xylem_tls_destroy_listener(xylem_tls_listener_t* ln) {
    if (!ln) {
        return;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_destroy_listener");

    tls_destroy_listener(&ln->internal);
}

int xylem_tls_read(xylem_tls_conn_t* tls, void* buf, int len) {
    if (!tls) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_read");

    if (!buf || len <= 0) {
        return -1;
    }

    return tls_read(&tls->internal, buf, len);
}

int xylem_tls_write(xylem_tls_conn_t* tls, const void* data, int len) {
    if (!tls) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_write");

    if (len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }

    return tls_write(&tls->internal, data, len);
}

void xylem_tls_set_read_deadline(xylem_tls_conn_t* tls, uint64_t deadline_ms) {
    if (tls) {
        RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_set_read_deadline");

        tls_set_read_deadline(&tls->internal, deadline_ms);
    }
}

void xylem_tls_set_write_deadline(xylem_tls_conn_t* tls, uint64_t deadline_ms) {
    if (tls) {
        RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_set_write_deadline");

        tls_set_write_deadline(&tls->internal, deadline_ms);
    }
}

int xylem_tls_remote_addr(
    xylem_tls_conn_t* tls,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    if (!tls) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_remote_addr");

    return tls_remote_addr(&tls->internal, host, host_len, port);
}

int xylem_tls_local_addr(
    xylem_tls_conn_t* tls,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    if (!tls) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_local_addr");

    return tls_local_addr(&tls->internal, host, host_len, port);
}

int xylem_tls_listener_addr(
    xylem_tls_listener_t* ln,
    char*                 host,
    size_t                host_len,
    uint16_t*             port) {
    if (!ln) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_listener_addr");

    return tls_listener_addr(&ln->internal, host, host_len, port);
}

const char* xylem_tls_get_alpn(xylem_tls_conn_t* tls) {
    if (!tls) {
        return NULL;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_get_alpn");

    return tls_get_alpn(&tls->internal);
}

int xylem_tls_handshake(xylem_tls_conn_t* tls) {
    if (!tls) {
        return -1;
    }
    RUNTIME_REQUIRE_COROUTINE("tls", "xylem_tls_handshake");

    return tls_handshake(&tls->internal);
}
