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
 * wolfSSL TLS/DTLS backend.
 *
 * wolfSSL exposes an OpenSSL-compatibility layer (built with
 * --enable-opensslextra / --enable-opensslall), so the TLS path here is a
 * near-mirror of tls-backend-openssl.c: the same SSL_CTX / SSL / memory-BIO
 * model, the same SNI table, ALPN wire encoding, verify flags, and keylog.
 * This file is a sibling to tls-backend-openssl.c; the build compiles
 * exactly one of them (XYLEM_TLS_BACKEND).
 *
 * DTLS divergence from the OpenSSL backend. wolfSSL does NOT expose
 * OpenSSL's cookie-callback API (SSL_CTX_set_cookie_generate_cb /
 * verify_cb). Instead it computes and verifies the HelloVerifyRequest
 * cookie internally from the peer address bound via wolfSSL_dtls_set_peer.
 * So the OpenSSL backend's HMAC-over-peer-address cookie machinery
 * (RAND_bytes secret + xylem_hmac256 + gen/verify callbacks) is gone here;
 * dtls_backend_conn_set_peer_addr simply hands wolfSSL the peer. MTU uses
 * the native wolfSSL_dtls_set_mtu; the retransmit timer uses the
 * compat-layer DTLSv1_get_timeout / DTLSv1_handle_timeout, which keep
 * OpenSSL's "return 0/false when no timeout is pending" contract the engine
 * relies on.
 *
 * Required wolfSSL build features (configure flags):
 *   --enable-opensslextra (or --enable-opensslall)  OpenSSL compat layer
 *   --enable-tls13                                   TLS 1.3
 *   --enable-dtls --enable-dtls13                    DTLS 1.2/1.3
 *   --enable-alpn                                    ALPN
 *   --enable-sni                                     SNI
 *   --enable-supportedcurves                         SSL_CTX_set1_groups_list
 *   --enable-keylog-export (CFLAGS -DHAVE_SECRET_CALLBACK)  keylog (optional)
 * wolfSSL >= 5.x is assumed (SSL_CTX_set_alpn_select_cb /
 * SSL_select_next_proto compat). The generated wolfssl/options.h must be on
 * the include path; it is included first so every wolfSSL header sees the
 * same build configuration.
 */

#include <wolfssl/options.h>

#include "net/tls/tls-backend.h"

#include "xylem/xylem-logger.h"

#include "platform/platform-io.h"
#include "platform/platform-socket.h"
#include "platform/platform-string.h"
#include "xylem/xylem-threads.h"

#include <wolfssl/openssl/bio.h>
#include <wolfssl/openssl/err.h>
#include <wolfssl/openssl/pem.h>
#include <wolfssl/openssl/ssl.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * The engine (tls.c) chunks plaintext writes to a backend-neutral 16 KiB
 * (TLS_MAX_PLAINTEXT) so the backend holds at most one record of
 * ciphertext at a time. Assert that constant matches the library's own
 * per-record plaintext cap, so a chunk never spills into a second record.
 * wolfSSL's OpenSSL compat layer does not always define
 * SSL3_RT_MAX_PLAIN_LENGTH, so guard the check.
 */
#ifdef SSL3_RT_MAX_PLAIN_LENGTH
_Static_assert(SSL3_RT_MAX_PLAIN_LENGTH == 16 * 1024,
               "TLS_MAX_PLAINTEXT must match TLS record plaintext cap");
#endif

typedef struct _wssl_sni_entry_s {
    char            hostname[256];
    X509*           cert;
    EVP_PKEY*       key;
    STACK_OF(X509)* chain;
} _wssl_sni_entry_t;

struct tls_backend_ctx_s {
    SSL_CTX*            ssl_ctx;
    tls_backend_proto_t proto;
    uint8_t*           alpn_wire;
    size_t             alpn_wire_len;
    FILE*              keylog_file;
    _wssl_sni_entry_t* sni_entries;
    size_t             sni_count;
    size_t             sni_cap;
};

struct tls_backend_conn_s {
    SSL*  ssl;
    BIO*  rbio;   /* inbound ciphertext: feed() -> SSL */
    BIO*  wbio;   /* outbound ciphertext: SSL -> drain() */
    bool  is_dtls;
    struct sockaddr_storage peer;     /* DTLS server cookie binding */
    size_t                  peer_len;
};

static int _wssl_ctx_ex_idx = -1;  /* SSL_CTX -> tls_backend_ctx_t* (keylog) */
static once_flag _wssl_ex_once = ONCE_FLAG_INIT;

static void _wssl_init_ex(void) {
    _wssl_ctx_ex_idx = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

/**
 * SNI servername callback. Selects the per-host certificate by setting it
 * directly on the SSL connection -- the single ctx (keylog / ALPN / verify)
 * stays in force. When no host matches, the ctx default cert is untouched.
 */
static int _wssl_sni_cb(SSL* ssl, int* al, void* arg) {
    (void)al;
    tls_backend_ctx_t* ctx = (tls_backend_ctx_t*)arg;
    const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (!name) {
        return SSL_TLSEXT_ERR_OK;
    }
    for (size_t i = 0; i < ctx->sni_count; i++) {
        _wssl_sni_entry_t* e = &ctx->sni_entries[i];
        if (platform_strcasecmp(name, e->hostname) != 0) {
            continue;
        }
        if (SSL_use_certificate(ssl, e->cert) != 1
            || SSL_use_PrivateKey(ssl, e->key) != 1) {
            xylem_loge("<tls> sni apply cert failed host=%s", e->hostname);
            return SSL_TLSEXT_ERR_OK;
        }
        if (e->chain) {
            SSL_set1_chain(ssl, e->chain);
        }
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_OK;
}

#if defined(HAVE_SECRET_CALLBACK)
static void _wssl_keylog_cb(const SSL* ssl, const char* line) {
    SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
    tls_backend_ctx_t* ctx =
        (tls_backend_ctx_t*)SSL_CTX_get_ex_data(ssl_ctx, _wssl_ctx_ex_idx);
    if (ctx && ctx->keylog_file) {
        fprintf(ctx->keylog_file, "%s\n", line);
        fflush(ctx->keylog_file);
    }
}
#endif

static int _wssl_alpn_select_cb(
    SSL*                  ssl,
    const unsigned char** out,
    unsigned char*        outlen,
    const unsigned char*  in,
    unsigned int          inlen,
    void*                 arg) {
    (void)ssl;
    tls_backend_ctx_t* ctx = (tls_backend_ctx_t*)arg;

    if (SSL_select_next_proto((unsigned char**)out, outlen,
                              ctx->alpn_wire,
                              (unsigned int)ctx->alpn_wire_len,
                              in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    return SSL_TLSEXT_ERR_OK;
}

/**
 * Parse a TLS identity (leaf cert + intermediate chain + matching key)
 * from two already-open PEM BIOs. The cert BIO holds the leaf first; any
 * following certs form the chain. The BIOs are not freed here.
 */
static int _wssl_parse_pem_identity(
    BIO*             cbio,
    BIO*             kbio,
    X509**           out_cert,
    EVP_PKEY**       out_key,
    STACK_OF(X509)** out_chain) {
    *out_cert  = NULL;
    *out_key   = NULL;
    *out_chain = NULL;

    X509* leaf = PEM_read_bio_X509(cbio, NULL, NULL, NULL);
    if (!leaf) {
        xylem_loge("<tls> parse leaf cert failed");
        return -1;
    }

    STACK_OF(X509)* chain = NULL;
    for (;;) {
        X509* extra = PEM_read_bio_X509(cbio, NULL, NULL, NULL);
        if (!extra) {
            ERR_clear_error();   /* EOF is the only expected stop */
            break;
        }
        if (!chain) {
            chain = sk_X509_new_null();
            if (!chain) {
                X509_free(extra);
                X509_free(leaf);
                return -1;
            }
        }
        if (sk_X509_push(chain, extra) <= 0) {
            X509_free(extra);
            sk_X509_pop_free(chain, X509_free);
            X509_free(leaf);
            return -1;
        }
    }

    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(kbio, NULL, NULL, NULL);
    if (!pkey) {
        xylem_loge("<tls> parse private key failed");
        sk_X509_pop_free(chain, X509_free);
        X509_free(leaf);
        return -1;
    }

    if (X509_check_private_key(leaf, pkey) != 1) {
        xylem_loge("<tls> cert and key mismatch");
        EVP_PKEY_free(pkey);
        sk_X509_pop_free(chain, X509_free);
        X509_free(leaf);
        return -1;
    }

    *out_cert  = leaf;
    *out_key   = pkey;
    *out_chain = chain;
    return 0;
}

static int _wssl_load_pem_identity(
    const char*      cert_file,
    const char*      key_file,
    X509**           out_cert,
    EVP_PKEY**       out_key,
    STACK_OF(X509)** out_chain) {
    BIO* cbio = BIO_new_file(cert_file, "r");
    if (!cbio) {
        xylem_loge("<tls> open cert failed path=%s", cert_file);
        return -1;
    }
    BIO* kbio = BIO_new_file(key_file, "r");
    if (!kbio) {
        xylem_loge("<tls> open key failed path=%s", key_file);
        BIO_free(cbio);
        return -1;
    }
    int rc = _wssl_parse_pem_identity(cbio, kbio, out_cert, out_key, out_chain);
    BIO_free(cbio);
    BIO_free(kbio);
    return rc;
}

static int _wssl_load_pem_identity_mem(
    const void*      cert_pem,
    size_t           cert_len,
    const void*      key_pem,
    size_t           key_len,
    X509**           out_cert,
    EVP_PKEY**       out_key,
    STACK_OF(X509)** out_chain) {
    BIO* cbio = BIO_new_mem_buf(cert_pem, (int)cert_len);
    BIO* kbio = BIO_new_mem_buf(key_pem, (int)key_len);
    if (!cbio || !kbio) {
        BIO_free(cbio);
        BIO_free(kbio);
        return -1;
    }
    int rc = _wssl_parse_pem_identity(cbio, kbio, out_cert, out_key, out_chain);
    BIO_free(cbio);
    BIO_free(kbio);
    return rc;
}

static int _wssl_store_sni_identity(
    tls_backend_ctx_t* ctx,
    const char*        hostname,
    X509*              leaf,
    EVP_PKEY*          key,
    STACK_OF(X509)*    chain) {
    if (ctx->sni_count == ctx->sni_cap) {
        size_t new_cap = ctx->sni_cap == 0 ? 4 : ctx->sni_cap * 2;
        _wssl_sni_entry_t* entries = (_wssl_sni_entry_t*)realloc(
            ctx->sni_entries, new_cap * sizeof(_wssl_sni_entry_t));
        if (!entries) {
            EVP_PKEY_free(key);
            sk_X509_pop_free(chain, X509_free);
            X509_free(leaf);
            return -1;
        }
        ctx->sni_entries = entries;
        ctx->sni_cap     = new_cap;
    }

    _wssl_sni_entry_t* entry = &ctx->sni_entries[ctx->sni_count];
    snprintf(entry->hostname, sizeof(entry->hostname), "%s", hostname);
    entry->cert  = leaf;
    entry->key   = key;
    entry->chain = chain;
    ctx->sni_count++;
    return 0;
}

static int _wssl_apply_default_identity(
    tls_backend_ctx_t* ctx,
    X509*              leaf,
    EVP_PKEY*          key,
    STACK_OF(X509)*    chain) {
    if (SSL_CTX_use_certificate(ctx->ssl_ctx, leaf) != 1
        || SSL_CTX_use_PrivateKey(ctx->ssl_ctx, key) != 1) {
        return -1;
    }
    if (chain && SSL_CTX_set1_chain(ctx->ssl_ctx, chain) != 1) {
        return -1;
    }
    return 0;
}

static int _wssl_install_identity(
    tls_backend_ctx_t* ctx,
    const char*        hostname,
    X509*              leaf,
    EVP_PKEY*          key,
    STACK_OF(X509)*    chain) {
    if (hostname) {
        return _wssl_store_sni_identity(ctx, hostname, leaf, key, chain);
    }
    int rc = _wssl_apply_default_identity(ctx, leaf, key, chain);
    EVP_PKEY_free(key);
    sk_X509_pop_free(chain, X509_free);
    X509_free(leaf);
    return rc;
}

tls_backend_ctx_t* tls_backend_ctx_create(tls_backend_proto_t proto) {
    tls_backend_ctx_t* ctx = (tls_backend_ctx_t*)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->proto = proto;

    /**
     * DTLS cookies are handled internally by wolfSSL from the peer address
     * bound via wolfSSL_dtls_set_peer (see dtls_backend_conn_set_peer_addr),
     * so unlike the OpenSSL backend there is no cookie secret / callback to
     * register here.
     */
    const SSL_METHOD* method =
        (proto == TLS_BACKEND_PROTO_DTLS) ? DTLS_method() : TLS_method();
    ctx->ssl_ctx = SSL_CTX_new(method);
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    call_once(&_wssl_ex_once, _wssl_init_ex);
    SSL_CTX_set_ex_data(ctx->ssl_ctx, _wssl_ctx_ex_idx, ctx);

    SSL_CTX_set_tlsext_servername_callback(ctx->ssl_ctx, _wssl_sni_cb);
    SSL_CTX_set_tlsext_servername_arg(ctx->ssl_ctx, ctx);

    if (proto == TLS_BACKEND_PROTO_DTLS) {
#ifdef SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
        SSL_CTX_set_mode(ctx->ssl_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#endif
        SSL_CTX_set_min_proto_version(ctx->ssl_ctx, DTLS1_2_VERSION);
    } else {
        SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION);
    }

    /* Classic curves over a post-quantum hybrid for faster handshakes. */
    tls_backend_ctx_set_kx_groups(ctx, "X25519:P-256:P-384:P-521");
    return ctx;
}

void tls_backend_ctx_destroy(tls_backend_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    for (size_t i = 0; i < ctx->sni_count; i++) {
        X509_free(ctx->sni_entries[i].cert);
        EVP_PKEY_free(ctx->sni_entries[i].key);
        sk_X509_pop_free(ctx->sni_entries[i].chain, X509_free);
    }
    free(ctx->sni_entries);
    if (ctx->keylog_file) {
        fclose(ctx->keylog_file);
    }
    SSL_CTX_free(ctx->ssl_ctx);
    free(ctx->alpn_wire);
    free(ctx);
}

int tls_backend_ctx_load_cert_file(
    tls_backend_ctx_t* ctx,
    const char*        hostname,
    const char*        cert_file,
    const char*        key_file) {
    X509*           leaf  = NULL;
    EVP_PKEY*       pkey  = NULL;
    STACK_OF(X509)* chain = NULL;
    if (_wssl_load_pem_identity(cert_file, key_file, &leaf, &pkey, &chain)
        != 0) {
        return -1;
    }
    return _wssl_install_identity(ctx, hostname, leaf, pkey, chain);
}

int tls_backend_ctx_load_cert_mem(
    tls_backend_ctx_t* ctx,
    const char*        hostname,
    const void*        cert_pem,
    size_t             cert_len,
    const void*        key_pem,
    size_t             key_len) {
    if (!cert_pem || cert_len == 0 || !key_pem || key_len == 0) {
        return -1;
    }
    X509*           leaf  = NULL;
    EVP_PKEY*       pkey  = NULL;
    STACK_OF(X509)* chain = NULL;
    if (_wssl_load_pem_identity_mem(cert_pem, cert_len, key_pem, key_len,
                                    &leaf, &pkey, &chain) != 0) {
        return -1;
    }
    return _wssl_install_identity(ctx, hostname, leaf, pkey, chain);
}

int tls_backend_ctx_load_ca_file(tls_backend_ctx_t* ctx, const char* ca_file) {
    if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_file, NULL) != 1) {
        xylem_loge("<tls> load ca failed path=%s", ca_file);
        return -1;
    }
    return 0;
}

/**
 * Load the platform's native system trust store, if reachable on a path the
 * library can read. wolfSSL has no Windows ROOT-store loader (no winstore),
 * so on Windows this returns false and the caller's fallback CA bundle is
 * required. On Linux/macOS the default verify paths resolve to the system
 * bundle when wolfSSL was built with --enable-sys-ca-certs.
 *
 * Order matters: __ANDROID__ implies __linux__ under the NDK, so the mobile
 * branch must precede the generic Unix branch.
 */
static bool _wssl_load_native_system_ca(tls_backend_ctx_t* ctx) {
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(_WIN32)
    (void)ctx;
    return false;   /* no winstore loader; bundle a fallback CA file */
#elif defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    (void)ctx;
    return false;
#else
    return SSL_CTX_set_default_verify_paths(ctx->ssl_ctx) == 1;
#endif
}

int tls_backend_ctx_load_system_ca(
    tls_backend_ctx_t* ctx,
    const char*        fallback_ca_file) {
    bool has_system = _wssl_load_native_system_ca(ctx);

    bool has_fallback = false;
    if (fallback_ca_file) {
        has_fallback = (SSL_CTX_load_verify_locations(
                            ctx->ssl_ctx, fallback_ca_file, NULL) == 1);
        if (!has_fallback) {
            xylem_loge("<tls> load fallback ca failed path=%s",
                       fallback_ca_file);
        }
    }

    if (!has_system && !has_fallback) {
        xylem_loge("<tls> load system ca failed: no system store and "
                   "no usable fallback (bundle a CA file, e.g. curl's "
                   "cacert.pem)");
        return -1;
    }
    return 0;
}

int tls_backend_ctx_set_alpn(
    tls_backend_ctx_t* ctx,
    const char**       protocols,
    size_t             count) {
    if (count == 0) {
        return -1;
    }

    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += 1 + strlen(protocols[i]);
    }

    uint8_t* wire = (uint8_t*)malloc(total);
    if (!wire) {
        return -1;
    }

    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        size_t plen = strlen(protocols[i]);
        wire[off++] = (uint8_t)plen;
        memcpy(wire + off, protocols[i], plen);
        off += plen;
    }

    free(ctx->alpn_wire);
    ctx->alpn_wire     = wire;
    ctx->alpn_wire_len = total;

    /**
     * One list serves both roles: the client offers it, the server uses the
     * select cb to pick from it. The unused half is inert per role.
     */
    SSL_CTX_set_alpn_protos(ctx->ssl_ctx, wire, (unsigned int)total);
    SSL_CTX_set_alpn_select_cb(ctx->ssl_ctx, _wssl_alpn_select_cb, ctx);

    return 0;
}

int tls_backend_ctx_set_kx_groups(tls_backend_ctx_t* ctx, const char* groups) {
    if (!ctx || !groups || groups[0] == '\0') {
        return -1;
    }
    if (SSL_CTX_set1_groups_list(ctx->ssl_ctx, (char*)groups) != 1) {
        xylem_loge("<tls> set kx groups failed list=%s", groups);
        return -1;
    }
    return 0;
}

int tls_backend_ctx_set_keylog(tls_backend_ctx_t* ctx, const char* path) {
    if (!ctx) {
        return -1;
    }
#if defined(HAVE_SECRET_CALLBACK)
    if (ctx->keylog_file) {
        fclose(ctx->keylog_file);
        ctx->keylog_file = NULL;
    }
    if (!path) {
        SSL_CTX_set_keylog_callback(ctx->ssl_ctx, NULL);
        return 0;
    }
    ctx->keylog_file = platform_io_fopen(path, "a");
    if (!ctx->keylog_file) {
        return -1;
    }
    SSL_CTX_set_keylog_callback(ctx->ssl_ctx, _wssl_keylog_cb);
    return 0;
#else
    (void)path;
    xylem_loge("<tls> keylog unsupported: build wolfSSL with "
               "-DHAVE_SECRET_CALLBACK (--enable-keylog-export)");
    return -1;
#endif
}

tls_backend_conn_t* tls_backend_conn_create(
    tls_backend_ctx_t* ctx,
    bool               is_server) {
    tls_backend_conn_t* c = (tls_backend_conn_t*)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->is_dtls = (ctx->proto == TLS_BACKEND_PROTO_DTLS);
    c->ssl = SSL_new(ctx->ssl_ctx);
    if (!c->ssl) {
        free(c);
        return NULL;
    }
    c->rbio = BIO_new(BIO_s_mem());
    c->wbio = BIO_new(BIO_s_mem());
    if (!c->rbio || !c->wbio) {
        BIO_free(c->rbio);
        BIO_free(c->wbio);
        SSL_free(c->ssl);
        free(c);
        return NULL;
    }
    SSL_set_bio(c->ssl, c->rbio, c->wbio);   /* SSL owns both BIOs now */

    /**
     * The engine pumps ciphertext through memory BIOs and owns the
     * retransmit timer, so wolfSSL must not run its own blocking/timeout
     * loop on the (BIO-backed) transport. Mark DTLS connections non-blocking
     * so wolfSSL surfaces WANT_READ/WANT_WRITE instead.
     */
    if (c->is_dtls) {
        wolfSSL_dtls_set_using_nonblock(c->ssl, 1);
    }

    if (is_server) {
        SSL_set_accept_state(c->ssl);
    } else {
        SSL_set_connect_state(c->ssl);
    }
    return c;
}

void tls_backend_conn_destroy(tls_backend_conn_t* c) {
    if (!c) {
        return;
    }
    if (c->ssl) {
        SSL_free(c->ssl);   /* frees the bound BIOs too */
    }
    free(c);
}

void tls_backend_conn_configure(
    tls_backend_conn_t*                c,
    const tls_backend_handshake_cfg_t* cfg) {
    int mode;
    switch (cfg->verify) {
        case TLS_BACKEND_VERIFY_REQUIRE:
            mode = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
            break;
        case TLS_BACKEND_VERIFY_PEER:
            mode = SSL_VERIFY_PEER;
            break;
        default:
            mode = SSL_VERIFY_NONE;
            break;
    }
    SSL_set_verify(c->ssl, mode, NULL);

    if (cfg->sni_name) {
        SSL_set_tlsext_host_name(c->ssl, cfg->sni_name);
    }
    if (cfg->verify_host) {
        SSL_set1_host(c->ssl, cfg->verify_host);   /* copies */
    }
}

int tls_backend_conn_feed(tls_backend_conn_t* c, const void* buf, int len) {
    return BIO_write(c->rbio, buf, len) == len ? 0 : -1;
}

int tls_backend_conn_drain(tls_backend_conn_t* c, void* buf, int cap) {
    int n = BIO_read(c->wbio, buf, cap);
    if (n > 0) {
        return n;
    }
    /**
     * A mem BIO with no pending bytes returns <=0 with the retry flag set;
     * that is "empty", not an error. Only a non-retry negative is a hard
     * failure.
     */
    return BIO_should_retry(c->wbio) ? 0 : (n < 0 ? -1 : 0);
}

static tls_backend_state_t _wssl_state(SSL* ssl, int ret) {
    if (ret == 1) {
        return TLS_BACKEND_OK;
    }
    int err = SSL_get_error(ssl, ret);
    switch (err) {
        case SSL_ERROR_WANT_READ:   return TLS_BACKEND_WANT_READ;
        case SSL_ERROR_WANT_WRITE:  return TLS_BACKEND_WANT_WRITE;
        case SSL_ERROR_ZERO_RETURN: return TLS_BACKEND_CLOSED;
        default: {
            unsigned long e = ERR_peek_error();
            xylem_loge("<tls> ssl op failed ssl_err=%d reason=%s", err,
                       ERR_reason_error_string(e)
                           ? ERR_reason_error_string(e) : "unknown");
            /**
             * Drain the queue on the error path so it is empty again for the
             * next op; this is what lets the hot read/write paths skip the
             * per-call ERR_clear_error.
             */
            ERR_clear_error();
            return TLS_BACKEND_ERROR;
        }
    }
}

tls_backend_state_t tls_backend_conn_handshake(tls_backend_conn_t* c) {
    ERR_clear_error();
    int ret = SSL_do_handshake(c->ssl);
    return _wssl_state(c->ssl, ret);
}

tls_backend_state_t tls_backend_conn_read(
    tls_backend_conn_t* c,
    void*               buf,
    int                 len,
    int*                out_n) {
    /* No ERR_clear_error here: _wssl_state drains the queue on the error
     * path, so it is already empty on entry. */
    int n = SSL_read(c->ssl, buf, len);
    if (n > 0) {
        *out_n = n;
        return TLS_BACKEND_OK;
    }
    *out_n = 0;
    return _wssl_state(c->ssl, n);
}

tls_backend_state_t tls_backend_conn_write(
    tls_backend_conn_t* c,
    const void*         buf,
    int                 len,
    int*                out_n) {
    int n = SSL_write(c->ssl, buf, len);
    if (n > 0) {
        *out_n = n;
        return TLS_BACKEND_OK;
    }
    *out_n = 0;
    return _wssl_state(c->ssl, n);
}

void tls_backend_conn_shutdown(tls_backend_conn_t* c) {
    if (c->ssl) {
        ERR_clear_error();
        SSL_shutdown(c->ssl);   /* queues close_notify into wbio */
    }
}

void tls_backend_conn_get_alpn(tls_backend_conn_t* c, char* buf, size_t cap) {
    const unsigned char* proto = NULL;
    unsigned int         plen  = 0;
    SSL_get0_alpn_selected(c->ssl, &proto, &plen);
    if (cap == 0) {
        return;
    }
    buf[0] = '\0';
    if (proto && plen > 0 && (size_t)plen < cap) {
        memcpy(buf, proto, plen);
        buf[plen] = '\0';
    }
}

/* ---------------------------------------------------------------------------
 * DTLS. wolfSSL handles HelloVerifyRequest cookies internally from the peer
 * address bound below, so there is no cookie-callback plumbing here.
 * ------------------------------------------------------------------------ */

void dtls_backend_conn_set_mtu(tls_backend_conn_t* c, uint16_t mtu) {
    if (mtu == 0) {
        return;
    }
    wolfSSL_dtls_set_mtu(c->ssl, mtu);
}

void dtls_backend_conn_set_peer_addr(
    tls_backend_conn_t* c,
    const void*         sockaddr,
    size_t              salen) {
    if (salen > sizeof(c->peer)) {
        salen = sizeof(c->peer);
    }
    memcpy(&c->peer, sockaddr, salen);
    c->peer_len = salen;
    /* Bind the peer so wolfSSL can generate/verify the DTLS cookie. */
    wolfSSL_dtls_set_peer(c->ssl, &c->peer, (unsigned int)c->peer_len);
}

bool dtls_backend_conn_get_timeout(tls_backend_conn_t* c, uint64_t* out_ms) {
    struct timeval tv;
    if (!DTLSv1_get_timeout(c->ssl, &tv)) {
        return false;
    }
    uint64_t ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
    *out_ms = (ms == 0) ? 1 : ms;
    return true;
}

void dtls_backend_conn_handle_timeout(tls_backend_conn_t* c) {
    DTLSv1_handle_timeout(c->ssl);
}
