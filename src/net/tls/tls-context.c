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

#include "tls-context.h"

#include "xylem/xylem-logger.h"

#include "net/addr.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    tls_backend_ctx_t* be;
    bool               verify_server;
    bool               verify_client;
} _tls_ctx_base_t;

struct xylem_tls_ctx_s {
    _tls_ctx_base_t base;
};

struct xylem_dtls_ctx_s {
    _tls_ctx_base_t base;
};

static int _tls_ctx_base_build_client_config(
    _tls_ctx_base_t*             ctx,
    const char*                  identity,
    const char*                  module,
    tls_backend_handshake_cfg_t* cfg) {
    cfg->verify =
        ctx->verify_server ? TLS_BACKEND_VERIFY_PEER : TLS_BACKEND_VERIFY_NONE;
    bool verify_peer = (cfg->verify != TLS_BACKEND_VERIFY_NONE);

    if (!identity && verify_peer) {
        xylem_loge(
            "<%s> peer identity unchecked verify=enabled risk=mitm",
            module);
    }
    if (!identity) {
        return 0;
    }

    size_t identity_len = strlen(identity);
    if (identity_len >= sizeof(cfg->identity)) {
        return -1;
    }
    memcpy(cfg->identity, identity, identity_len + 1);
    cfg->identity_type = TLS_BACKEND_IDENTITY_DNS;

    addr_t tmp;
    char*  zone = strchr(cfg->identity, '%');
    if (!zone || zone == cfg->identity || !zone[1]
        || !strchr(cfg->identity, ':')) {
        zone = NULL;
    }
    if (zone) {
        /* A scope selects an interface, not a certificate identity. */
        *zone = '\0';
    }
    if (addr_pton(cfg->identity, 0, &tmp) == 0) {
        cfg->identity_type = TLS_BACKEND_IDENTITY_IP;
        return 0;
    }
    if (zone) {
        *zone = '%';
    }
    if (identity_len > 1 && cfg->identity[identity_len - 1] == '.') {
        cfg->identity[identity_len - 1] = '\0';
    }
    return 0;
}

static void _tls_ctx_base_build_server_config(
    _tls_ctx_base_t*             ctx,
    tls_backend_handshake_cfg_t* cfg) {
    cfg->verify = ctx->verify_client ? TLS_BACKEND_VERIFY_REQUIRE
                                     : TLS_BACKEND_VERIFY_NONE;
}

static int _tls_ctx_base_init(
    _tls_ctx_base_t*    base,
    tls_backend_proto_t proto) {
    base->be = tls_backend_ctx_create(proto);
    if (!base->be) {
        return -1;
    }
    base->verify_server = true;
    base->verify_client = false;
    return 0;
}

static void _tls_ctx_base_deinit(_tls_ctx_base_t* base) {
    tls_backend_ctx_destroy(base->be);
}

static int _tls_ctx_base_set_keylog(
    _tls_ctx_base_t* base,
    const char*      path) {
    return tls_backend_ctx_set_keylog(base->be, path);
}

static int _tls_ctx_base_load_cert(
    _tls_ctx_base_t* base,
    const char*      hostname,
    const char*      cert,
    const char*      key) {
    return tls_backend_ctx_load_cert_file(base->be, hostname, cert, key);
}

static int _tls_ctx_base_load_cert_mem(
    _tls_ctx_base_t* base,
    const char*      hostname,
    const void*      cert_pem,
    size_t           cert_len,
    const void*      key_pem,
    size_t           key_len) {
    if (!cert_pem || cert_len == 0 || !key_pem || key_len == 0) {
        return -1;
    }
    return tls_backend_ctx_load_cert_mem(base->be, hostname, cert_pem, cert_len,
                                         key_pem, key_len);
}

static int _tls_ctx_base_load_ca(
    _tls_ctx_base_t* base,
    const char*      ca_file) {
    return tls_backend_ctx_load_ca_file(base->be, ca_file);
}

static int _tls_ctx_base_load_system_ca(
    _tls_ctx_base_t* base,
    const char*      fallback_ca_file) {
    return tls_backend_ctx_load_system_ca(base->be, fallback_ca_file);
}

static void _tls_ctx_base_verify_server(
    _tls_ctx_base_t* base,
    bool             enable) {
    base->verify_server = enable;
}

static void _tls_ctx_base_verify_client(
    _tls_ctx_base_t* base,
    bool             enable) {
    base->verify_client = enable;
}

static int _tls_ctx_base_set_alpn(
    _tls_ctx_base_t* base,
    const char**     protocols,
    size_t           count) {
    return tls_backend_ctx_set_alpn(base->be, protocols, count);
}

tls_ctx_t* tls_ctx_create(void) {
    tls_ctx_t* ctx = (tls_ctx_t*)calloc(1, sizeof(tls_ctx_t));
    if (!ctx) {
        return NULL;
    }
    if (_tls_ctx_base_init(&ctx->base, TLS_BACKEND_PROTO_TLS) != 0) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

dtls_ctx_t* dtls_ctx_create(void) {
    dtls_ctx_t* ctx = (dtls_ctx_t*)calloc(1, sizeof(dtls_ctx_t));
    if (!ctx) {
        return NULL;
    }
    if (_tls_ctx_base_init(&ctx->base, TLS_BACKEND_PROTO_DTLS) != 0) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

void tls_ctx_destroy(tls_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    _tls_ctx_base_deinit(&ctx->base);
    free(ctx);
}

void dtls_ctx_destroy(dtls_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    _tls_ctx_base_deinit(&ctx->base);
    free(ctx);
}

int tls_ctx_set_keylog(tls_ctx_t* ctx, const char* path) {
    return ctx ? _tls_ctx_base_set_keylog(&ctx->base, path) : -1;
}

int dtls_ctx_set_keylog(dtls_ctx_t* ctx, const char* path) {
    return ctx ? _tls_ctx_base_set_keylog(&ctx->base, path) : -1;
}

int tls_ctx_load_cert(
    tls_ctx_t*  ctx,
    const char* hostname,
    const char* cert,
    const char* key) {
    return _tls_ctx_base_load_cert(&ctx->base, hostname, cert, key);
}

int dtls_ctx_load_cert(
    dtls_ctx_t* ctx,
    const char* hostname,
    const char* cert,
    const char* key) {
    return _tls_ctx_base_load_cert(&ctx->base, hostname, cert, key);
}

int tls_ctx_load_cert_mem(
    tls_ctx_t*  ctx,
    const char* hostname,
    const void* cert_pem,
    size_t      cert_len,
    const void* key_pem,
    size_t      key_len) {
    return _tls_ctx_base_load_cert_mem(&ctx->base, hostname, cert_pem, cert_len,
                                       key_pem, key_len);
}

int dtls_ctx_load_cert_mem(
    dtls_ctx_t* ctx,
    const char* hostname,
    const void* cert_pem,
    size_t      cert_len,
    const void* key_pem,
    size_t      key_len) {
    return _tls_ctx_base_load_cert_mem(&ctx->base, hostname, cert_pem, cert_len,
                                       key_pem, key_len);
}

int tls_ctx_load_ca(tls_ctx_t* ctx, const char* ca_file) {
    return _tls_ctx_base_load_ca(&ctx->base, ca_file);
}

int dtls_ctx_load_ca(dtls_ctx_t* ctx, const char* ca_file) {
    return _tls_ctx_base_load_ca(&ctx->base, ca_file);
}

int tls_ctx_load_system_ca(tls_ctx_t* ctx, const char* fallback_ca_file) {
    return _tls_ctx_base_load_system_ca(&ctx->base, fallback_ca_file);
}

int dtls_ctx_load_system_ca(
    dtls_ctx_t* ctx,
    const char* fallback_ca_file) {
    return _tls_ctx_base_load_system_ca(&ctx->base, fallback_ca_file);
}

void tls_ctx_verify_server(tls_ctx_t* ctx, bool enable) {
    _tls_ctx_base_verify_server(&ctx->base, enable);
}

void dtls_ctx_verify_server(dtls_ctx_t* ctx, bool enable) {
    _tls_ctx_base_verify_server(&ctx->base, enable);
}

void tls_ctx_verify_client(tls_ctx_t* ctx, bool enable) {
    _tls_ctx_base_verify_client(&ctx->base, enable);
}

void dtls_ctx_verify_client(dtls_ctx_t* ctx, bool enable) {
    _tls_ctx_base_verify_client(&ctx->base, enable);
}

int tls_ctx_set_alpn(tls_ctx_t* ctx, const char** protocols, size_t count) {
    return _tls_ctx_base_set_alpn(&ctx->base, protocols, count);
}

int dtls_ctx_set_alpn(
    dtls_ctx_t* ctx,
    const char** protocols,
    size_t       count) {
    return _tls_ctx_base_set_alpn(&ctx->base, protocols, count);
}

tls_backend_ctx_t* tls_ctx_get_backend(tls_ctx_t* ctx) {
    return ctx->base.be;
}

tls_backend_ctx_t* dtls_ctx_get_backend(dtls_ctx_t* ctx) {
    return ctx->base.be;
}

int tls_ctx_build_client_config(
    tls_ctx_t*                   ctx,
    const char*                  identity,
    const char*                  module,
    tls_backend_handshake_cfg_t* cfg) {
    return _tls_ctx_base_build_client_config(&ctx->base, identity, module, cfg);
}

int dtls_ctx_build_client_config(
    dtls_ctx_t*                  ctx,
    const char*                  identity,
    const char*                  module,
    tls_backend_handshake_cfg_t* cfg) {
    return _tls_ctx_base_build_client_config(&ctx->base, identity, module, cfg);
}

void tls_ctx_build_server_config(
    tls_ctx_t*                   ctx,
    tls_backend_handshake_cfg_t* cfg) {
    _tls_ctx_base_build_server_config(&ctx->base, cfg);
}

void dtls_ctx_build_server_config(
    dtls_ctx_t*                  ctx,
    tls_backend_handshake_cfg_t* cfg) {
    _tls_ctx_base_build_server_config(&ctx->base, cfg);
}