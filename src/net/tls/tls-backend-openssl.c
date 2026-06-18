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

#include "net/tls/tls-backend.h"

#include "xylem/xylem-logger.h"
#include "xylem/crypto/xylem-hmac256.h"

#include "platform/platform-io.h"
#include "platform/platform-socket.h"
#include "platform/platform-string.h"
#include "platform/platform-tls.h"
#include "xylem/xylem-threads.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TLSB_COOKIE_SIZE        32
#define TLSB_DEFAULT_DGRAM_MTU  1500

/**
 * The engine (tls.c) chunks plaintext writes to a backend-neutral 16 KiB
 * (TLS_MAX_PLAINTEXT) so the backend holds at most one record of
 * ciphertext at a time. Assert that constant matches OpenSSL's own
 * per-record plaintext cap, so a chunk never spills into a second record.
 */
_Static_assert(SSL3_RT_MAX_PLAIN_LENGTH == 16 * 1024,
               "TLS_MAX_PLAINTEXT must match OpenSSL record plaintext cap");

typedef struct _tlsb_sni_entry_s {
    char            hostname[256];
    X509*           cert;
    EVP_PKEY*       key;
    STACK_OF(X509)* chain;
} _tlsb_sni_entry_t;

struct tls_backend_ctx_s {
    SSL_CTX*            ssl_ctx;
    tls_backend_proto_t proto;
    uint8_t*           alpn_wire;
    size_t             alpn_wire_len;
    FILE*              keylog_file;
    _tlsb_sni_entry_t* sni_entries;
    size_t             sni_count;
    size_t             sni_cap;
    uint8_t            cookie_secret[TLSB_COOKIE_SIZE]; /* DTLS only */
};

struct tls_backend_conn_s {
    SSL*             ssl;
    BIO*             rbio;
    BIO*             wbio;
    tls_backend_io_t io;
    uint16_t         mtu;
    struct sockaddr_storage peer;     /* DTLS server cookie binding */
    size_t                  peer_len;
};

static int _tlsb_ctx_ex_idx  = -1;  /* SSL_CTX -> tls_backend_ctx_t* */
static int _tlsb_conn_ex_idx = -1;  /* SSL     -> tls_backend_conn_t* (DTLS) */
static once_flag _tlsb_ex_once = ONCE_FLAG_INIT;
static BIO_METHOD* _tlsb_stream_bio_method;
static BIO_METHOD* _tlsb_dgram_bio_method;

static int _tlsb_io_bio_create(BIO* bio) {
    BIO_set_init(bio, 1);
    BIO_set_data(bio, NULL);
    return 1;
}

static int _tlsb_io_bio_destroy(BIO* bio) {
    if (!bio) {
        return 0;
    }
    BIO_set_data(bio, NULL);
    BIO_set_init(bio, 0);
    return 1;
}

static int _tlsb_io_bio_read(BIO* bio, char* out, int len) {
    tls_backend_conn_t* c = (tls_backend_conn_t*)BIO_get_data(bio);
    if (!c || !out || len <= 0) {
        return 0;
    }

    BIO_clear_retry_flags(bio);
    bool again = false;
    int  n     = c->io.read(c->io.user, out, len, &again);
    if (n < 0 && again) {
        BIO_set_retry_read(bio);
    }
    return n;
}

static int _tlsb_io_bio_write(BIO* bio, const char* in, int len) {
    tls_backend_conn_t* c = (tls_backend_conn_t*)BIO_get_data(bio);
    if (!c || !in || len <= 0) {
        return 0;
    }

    BIO_clear_retry_flags(bio);
    bool again = false;
    int  n     = c->io.write(c->io.user, in, len, &again);
    if (n < 0 && again) {
        BIO_set_retry_write(bio);
    }
    return n;
}

static long _tlsb_stream_bio_ctrl(
    BIO* bio,
    int  cmd,
    long num,
    void* ptr) {
    (void)bio;
    (void)num;
    (void)ptr;
    return cmd == BIO_CTRL_FLUSH ? 1 : 0;
}

static long _tlsb_dgram_bio_ctrl(
    BIO*  bio,
    int   cmd,
    long  num,
    void* ptr) {
    (void)ptr;
    tls_backend_conn_t* c = (tls_backend_conn_t*)BIO_get_data(bio);
    uint16_t mtu = (c && c->mtu > 0) ? c->mtu : TLSB_DEFAULT_DGRAM_MTU;

    switch (cmd) {
        case BIO_CTRL_FLUSH:
        case BIO_CTRL_DGRAM_SET_CONNECTED:
        case BIO_CTRL_DGRAM_SET_PEER:
        case BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT:
        case BIO_CTRL_DGRAM_MTU_DISCOVER:
        case BIO_CTRL_DGRAM_SET_DONT_FRAG:
            return 1;
        case BIO_CTRL_DGRAM_SET_MTU:
            if (c && num > 0) {
                c->mtu = (uint16_t)num;
                return 1;
            }
            return 0;
        case BIO_CTRL_DGRAM_QUERY_MTU:
        case BIO_CTRL_DGRAM_GET_MTU:
        case BIO_CTRL_DGRAM_GET_FALLBACK_MTU:
            return (long)mtu;
        case BIO_CTRL_DGRAM_MTU_EXCEEDED:
        case BIO_CTRL_DGRAM_GET_RECV_TIMER_EXP:
        case BIO_CTRL_DGRAM_GET_SEND_TIMER_EXP:
            return 0;
        case BIO_CTRL_DGRAM_GET_MTU_OVERHEAD:
            return 0;
        default:
            return 0;
    }
}

static bool _tlsb_conn_uses_callback_io(tls_backend_conn_t* c) {
    return c->io.read && c->io.write;
}

static void _tlsb_init_ex(void) {
    _tlsb_ctx_ex_idx  = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
    _tlsb_conn_ex_idx = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
    _tlsb_stream_bio_method = BIO_meth_new(
        BIO_TYPE_SOURCE_SINK,
        "xylem-stream");
    if (_tlsb_stream_bio_method) {
        BIO_meth_set_create(_tlsb_stream_bio_method,
                            _tlsb_io_bio_create);
        BIO_meth_set_destroy(_tlsb_stream_bio_method,
                             _tlsb_io_bio_destroy);
        BIO_meth_set_read(_tlsb_stream_bio_method, _tlsb_io_bio_read);
        BIO_meth_set_write(_tlsb_stream_bio_method, _tlsb_io_bio_write);
        BIO_meth_set_ctrl(_tlsb_stream_bio_method, _tlsb_stream_bio_ctrl);
    }
    _tlsb_dgram_bio_method = BIO_meth_new(BIO_TYPE_DGRAM, "xylem-dgram");
    if (_tlsb_dgram_bio_method) {
        BIO_meth_set_create(_tlsb_dgram_bio_method,
                            _tlsb_io_bio_create);
        BIO_meth_set_destroy(_tlsb_dgram_bio_method,
                             _tlsb_io_bio_destroy);
        BIO_meth_set_read(_tlsb_dgram_bio_method, _tlsb_io_bio_read);
        BIO_meth_set_write(_tlsb_dgram_bio_method, _tlsb_io_bio_write);
        BIO_meth_set_ctrl(_tlsb_dgram_bio_method, _tlsb_dgram_bio_ctrl);
    }
}

/**
 * SNI servername callback. Selects the per-host certificate by setting
 * it directly on the SSL connection -- the single ctx (and thus its
 * keylog / ALPN / verify config) stays in force. Mirrors Go's
 * GetCertificate / rustls' cert resolver: SNI only picks a cert, never
 * swaps the whole configuration. When no host matches, the ctx default
 * certificate is left untouched.
 */
static int _tlsb_ctx_sni_cb(SSL* ssl, int* al, void* arg) {
    (void)al;
    tls_backend_ctx_t* ctx = (tls_backend_ctx_t*)arg;
    const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (!name) {
        return SSL_TLSEXT_ERR_OK;
    }
    for (size_t i = 0; i < ctx->sni_count; i++) {
        _tlsb_sni_entry_t* e = &ctx->sni_entries[i];
        if (platform_strcasecmp(name, e->hostname) != 0) {
            continue;
        }
        /**
         * use/set1 variants bump the refcount on the stored objects,
         * so the ctx keeps ownership and each connection holds its own
         * reference. Validated key/cert pairing at load time.
         */
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

static void _tlsb_ssl_keylog_cb(const SSL* ssl, const char* line) {
    SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
    tls_backend_ctx_t* ctx =
        (tls_backend_ctx_t*)SSL_CTX_get_ex_data(ssl_ctx, _tlsb_ctx_ex_idx);
    if (ctx && ctx->keylog_file) {
        fprintf(ctx->keylog_file, "%s\n", line);
        fflush(ctx->keylog_file);
    }
}

static int _tlsb_alpn_select_cb(
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
 * from two already-open PEM BIOs. The cert BIO holds the leaf first,
 * any following certs form the chain. The BIOs are not freed here. On
 * success out_* are set (caller owns them); on failure nothing is left
 * allocated to the caller.
 */
static int _tlsb_parse_pem_identity(
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
            /* EOF is the only expected stop; clear the residual error. */
            ERR_clear_error();
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

    /* Reject a mismatched cert/key pair up front, not mid-handshake. */
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

/* Load a TLS identity from PEM files. See _tlsb_parse_pem_identity. */
static int _tlsb_load_pem_identity(
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
    int rc = _tlsb_parse_pem_identity(cbio, kbio, out_cert, out_key, out_chain);
    BIO_free(cbio);
    BIO_free(kbio);
    return rc;
}

/* Load a TLS identity from in-memory PEM buffers. */
static int _tlsb_load_pem_identity_mem(
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
    int rc = _tlsb_parse_pem_identity(cbio, kbio, out_cert, out_key, out_chain);
    BIO_free(cbio);
    BIO_free(kbio);
    return rc;
}

/**
 * Take ownership of (leaf, key, chain) into a new SNI entry bound to
 * hostname. On allocation failure the identity is freed and -1 is
 * returned, so the caller never has to clean up on error.
 */
static int _tlsb_store_sni_identity(
    tls_backend_ctx_t* ctx,
    const char*        hostname,
    X509*              leaf,
    EVP_PKEY*          key,
    STACK_OF(X509)*    chain) {
    if (ctx->sni_count == ctx->sni_cap) {
        size_t new_cap = ctx->sni_cap == 0 ? 4 : ctx->sni_cap * 2;
        _tlsb_sni_entry_t* entries = (_tlsb_sni_entry_t*)realloc(
            ctx->sni_entries, new_cap * sizeof(_tlsb_sni_entry_t));
        if (!entries) {
            EVP_PKEY_free(key);
            sk_X509_pop_free(chain, X509_free);
            X509_free(leaf);
            return -1;
        }
        ctx->sni_entries = entries;
        ctx->sni_cap     = new_cap;
    }

    _tlsb_sni_entry_t* entry = &ctx->sni_entries[ctx->sni_count];
    snprintf(entry->hostname, sizeof(entry->hostname), "%s", hostname);
    entry->cert  = leaf;
    entry->key   = key;
    entry->chain = chain;
    ctx->sni_count++;
    return 0;
}

/**
 * Install (leaf, key, chain) as the ctx default identity. use/set1 bump
 * the refcount on each object, so the caller keeps ownership of its own
 * references and must free them afterwards.
 */
static int _tlsb_apply_default_identity(
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

/**
 * Install a parsed identity into the ctx: as the default identity when
 * hostname is NULL, or as an SNI-selected identity otherwise. Always
 * consumes (leaf, key, chain) -- it takes ownership for the SNI case and
 * frees its references for the default case (which only bumps refcounts).
 */
static int _tlsb_install_identity(
    tls_backend_ctx_t* ctx,
    const char*        hostname,
    X509*              leaf,
    EVP_PKEY*          key,
    STACK_OF(X509)*    chain) {
    if (hostname) {
        return _tlsb_store_sni_identity(ctx, hostname, leaf, key, chain);
    }
    int rc = _tlsb_apply_default_identity(ctx, leaf, key, chain);
    EVP_PKEY_free(key);
    sk_X509_pop_free(chain, X509_free);
    X509_free(leaf);
    return rc;
}

static int _tlsb_cookie_peer(SSL* ssl, const uint8_t** out, size_t* out_len) {
    tls_backend_conn_t* c =
        (tls_backend_conn_t*)SSL_get_ex_data(ssl, _tlsb_conn_ex_idx);
    if (!c || c->peer_len == 0) {
        return -1;
    }
    *out     = (const uint8_t*)&c->peer;
    *out_len = c->peer_len;
    return 0;
}

static int _tlsb_cookie_generate_cb(
    SSL*           ssl,
    unsigned char* cookie,
    unsigned int*  cookie_len) {
    SSL_CTX* sc = SSL_get_SSL_CTX(ssl);
    tls_backend_ctx_t* ctx =
        (tls_backend_ctx_t*)SSL_CTX_get_ex_data(sc, _tlsb_ctx_ex_idx);
    const uint8_t* msg;
    size_t         msg_len;
    if (!ctx || _tlsb_cookie_peer(ssl, &msg, &msg_len) < 0) {
        return 0;
    }
    xylem_hmac256_compute(ctx->cookie_secret, sizeof(ctx->cookie_secret),
                          msg, msg_len, cookie);
    *cookie_len = TLSB_COOKIE_SIZE;
    return 1;
}

static int _tlsb_cookie_verify_cb(
    SSL*                 ssl,
    const unsigned char* cookie,
    unsigned int         cookie_len) {
    SSL_CTX* sc = SSL_get_SSL_CTX(ssl);
    tls_backend_ctx_t* ctx =
        (tls_backend_ctx_t*)SSL_CTX_get_ex_data(sc, _tlsb_ctx_ex_idx);
    const uint8_t* msg;
    size_t         msg_len;
    if (!ctx || _tlsb_cookie_peer(ssl, &msg, &msg_len) < 0) {
        return 0;
    }
    uint8_t expected[TLSB_COOKIE_SIZE];
    xylem_hmac256_compute(ctx->cookie_secret, sizeof(ctx->cookie_secret),
                          msg, msg_len, expected);
    if (cookie_len != TLSB_COOKIE_SIZE) {
        return 0;
    }
    return CRYPTO_memcmp(cookie, expected, TLSB_COOKIE_SIZE) == 0 ? 1 : 0;
}

tls_backend_ctx_t* tls_backend_ctx_create(tls_backend_proto_t proto) {
    tls_backend_ctx_t* ctx = (tls_backend_ctx_t*)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->proto = proto;

    const SSL_METHOD* method =
        (proto == TLS_BACKEND_PROTO_DTLS) ? DTLS_method() : TLS_method();
    ctx->ssl_ctx = SSL_CTX_new(method);
    if (!ctx->ssl_ctx) {
        free(ctx);
        return NULL;
    }

    call_once(&_tlsb_ex_once, _tlsb_init_ex);
    SSL_CTX_set_ex_data(ctx->ssl_ctx, _tlsb_ctx_ex_idx, ctx);

    SSL_CTX_set_tlsext_servername_callback(ctx->ssl_ctx, _tlsb_ctx_sni_cb);
    SSL_CTX_set_tlsext_servername_arg(ctx->ssl_ctx, ctx);

    if (proto == TLS_BACKEND_PROTO_DTLS) {
        if (RAND_bytes(ctx->cookie_secret, sizeof(ctx->cookie_secret)) != 1) {
            SSL_CTX_free(ctx->ssl_ctx);
            free(ctx);
            return NULL;
        }
        SSL_CTX_set_mode(ctx->ssl_ctx, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        SSL_CTX_set_cookie_generate_cb(ctx->ssl_ctx, _tlsb_cookie_generate_cb);
        SSL_CTX_set_cookie_verify_cb(ctx->ssl_ctx, _tlsb_cookie_verify_cb);
        SSL_CTX_set_min_proto_version(ctx->ssl_ctx, DTLS1_2_VERSION);
    } else {
        SSL_CTX_set_min_proto_version(ctx->ssl_ctx, TLS1_2_VERSION);
    }

    /**
     * Default to classic curves over OpenSSL's post-quantum hybrid for
     * faster handshakes; see tls_backend_ctx_set_kx_groups.
     */
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
    if (_tlsb_load_pem_identity(cert_file, key_file, &leaf, &pkey, &chain)
        != 0) {
        return -1;
    }
    return _tlsb_install_identity(ctx, hostname, leaf, pkey, chain);
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
    if (_tlsb_load_pem_identity_mem(cert_pem, cert_len, key_pem, key_len,
                                    &leaf, &pkey, &chain) != 0) {
        return -1;
    }
    return _tlsb_install_identity(ctx, hostname, leaf, pkey, chain);
}

int tls_backend_ctx_load_ca_file(tls_backend_ctx_t* ctx, const char* ca_file) {
    if (SSL_CTX_load_verify_locations(ctx->ssl_ctx, ca_file, NULL) != 1) {
        xylem_loge("<tls> load ca failed path=%s", ca_file);
        return -1;
    }
    return 0;
}

/**
 * Attempt to load the platform's native system trust store into ctx.
 * Returns true if the platform has an OpenSSL-readable system store and
 * the load call succeeded, false otherwise (including mobile, which has
 * no OpenSSL-readable store at all).
 *
 *   - Windows: the winstore loader (OpenSSL 3.2+) reads the system ROOT
 *     store on demand. OpenSSL's default verify paths are empty here.
 *   - Linux / macOS: OpenSSL's default verify paths resolve to the
 *     system/distribution CA bundle (or the bundle shipped alongside the
 *     linked OpenSSL, e.g. Homebrew on macOS).
 *   - Android / iOS: the CAs live behind the Java KeyStore /
 *     Security.framework, not on a path OpenSSL can load -> false.
 *
 * Platform conditionals live behind platform_tls_ca_store.
 */
static bool _tlsb_load_native_system_ca(tls_backend_ctx_t* ctx) {
    switch (platform_tls_ca_store()) {
    case PLATFORM_TLS_CA_STORE_NATIVE_WINDOWS:
        return SSL_CTX_load_verify_store(ctx->ssl_ctx,
                                         "org.openssl.winstore://") == 1;
    case PLATFORM_TLS_CA_STORE_DEFAULT_PATHS:
        return SSL_CTX_set_default_verify_paths(ctx->ssl_ctx) == 1;
    case PLATFORM_TLS_CA_STORE_NONE:
    default:
        return false;
    }
}

/**
 * Load trust anchors for verifying public CAs, combining (additively) the
 * platform system store with an optional fallback CA file:
 *
 *   1. Try the native system store (see _tlsb_load_native_system_ca).
 *   2. If fallback_ca_file is non-NULL, also load it as a PEM bundle.
 *
 * Anchors from both sources accumulate in the same X509_STORE, so the
 * call succeeds as long as *either* source loaded. This is deliberately
 * additive rather than "system, else fallback": OpenSSL's default-paths
 * CApath is lazy (consulted per-hash at verify time), so there is no
 * reliable way to detect whether the system store actually contained any
 * certificates -- loading both and taking the union sidesteps that.
 *
 * The fallback is what makes this usable where the native store is absent
 * or unreachable: mobile (Android/iOS), a statically linked or
 * cross-compiled OpenSSL whose build-time OPENSSLDIR does not exist on the
 * target, or a custom OpenSSL install with no CA bundle. Point it at a CA
 * file shipped with the app (e.g. curl's cacert.pem from
 * https://curl.se/ca/cacert.pem).
 */
int tls_backend_ctx_load_system_ca(
    tls_backend_ctx_t* ctx,
    const char*        fallback_ca_file) {
    bool has_system = _tlsb_load_native_system_ca(ctx);

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
     * One list serves both roles: the client offers it, the server uses
     * the select cb to pick from it. The unused half is inert per role.
     */
    SSL_CTX_set_alpn_protos(ctx->ssl_ctx, wire, (unsigned int)total);
    SSL_CTX_set_alpn_select_cb(ctx->ssl_ctx, _tlsb_alpn_select_cb, ctx);

    return 0;
}

int tls_backend_ctx_set_kx_groups(tls_backend_ctx_t* ctx, const char* groups) {
    if (!ctx || !groups || groups[0] == '\0') {
        return -1;
    }
    if (SSL_CTX_set1_groups_list(ctx->ssl_ctx, groups) != 1) {
        xylem_loge("<tls> set kx groups failed list=%s", groups);
        return -1;
    }
    return 0;
}

int tls_backend_ctx_set_keylog(tls_backend_ctx_t* ctx, const char* path) {
    if (!ctx) {
        return -1;
    }
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
    SSL_CTX_set_keylog_callback(ctx->ssl_ctx, _tlsb_ssl_keylog_cb);
    return 0;
}

tls_backend_conn_t* tls_backend_conn_create(
    tls_backend_ctx_t* ctx,
    bool               is_server,
    const tls_backend_io_t* io) {
    tls_backend_conn_t* c = (tls_backend_conn_t*)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->ssl = SSL_new(ctx->ssl_ctx);
    if (!c->ssl) {
        free(c);
        return NULL;
    }
    bool use_callback_io = io && io->read && io->write;
    if (use_callback_io) {
        BIO_METHOD* method = (ctx->proto == TLS_BACKEND_PROTO_DTLS)
            ? _tlsb_dgram_bio_method
            : _tlsb_stream_bio_method;
        if (!method) {
            SSL_free(c->ssl);
            free(c);
            return NULL;
        }
        c->io   = *io;
        c->rbio = BIO_new(method);
        if (!c->rbio || BIO_up_ref(c->rbio) != 1) {
            BIO_free(c->rbio);
            SSL_free(c->ssl);
            free(c);
            return NULL;
        }
        BIO_set_data(c->rbio, c);
        c->wbio = c->rbio;
    } else {
        c->rbio = BIO_new(BIO_s_mem());
        c->wbio = BIO_new(BIO_s_mem());
    }
    if (!c->rbio || !c->wbio) {
        BIO_free(c->rbio);
        if (c->wbio != c->rbio) {
            BIO_free(c->wbio);
        }
        SSL_free(c->ssl);
        free(c);
        return NULL;
    }
    SSL_set_bio(c->ssl, c->rbio, c->wbio);   /* SSL owns both BIOs now */

    /* DTLS server cookie path needs SSL -> conn lookup. */
    SSL_set_ex_data(c->ssl, _tlsb_conn_ex_idx, c);

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
    if (_tlsb_conn_uses_callback_io(c)) {
        return -1;
    }
    return BIO_write(c->rbio, buf, len) == len ? 0 : -1;
}

int tls_backend_conn_drain(tls_backend_conn_t* c, void* buf, int cap) {
    if (_tlsb_conn_uses_callback_io(c)) {
        return 0;
    }
    int n = BIO_read(c->wbio, buf, cap);
    if (n > 0) {
        return n;
    }
    /**
     * A mem BIO with no pending bytes returns <=0 with the retry flag
     * set; that is "empty", not an error. Only a non-retry negative is a
     * hard failure.
     */
    return BIO_should_retry(c->wbio) ? 0 : (n < 0 ? -1 : 0);
}

static tls_backend_state_t _tlsb_state(SSL* ssl, int ret) {
    /**
     * Handshake success: SSL_do_handshake returns 1 (read/write map
     * their own >0 before this). Else ret==1 -> SSL_ERROR_NONE -> false
     * error.
     */
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
             * Drain the queue on the error path so it is empty again for
             * the next op. This is what lets the hot read/write paths skip
             * the per-call ERR_clear_error: a successful SSL_read/SSL_write
             * adds nothing to the queue, and WANT_READ/WANT_WRITE are
             * decided from the SSL rwstate (not the queue), so the only way
             * to leave a stale entry is a real error -- cleared right here.
             */
            ERR_clear_error();
            return TLS_BACKEND_ERROR;
        }
    }
}

tls_backend_state_t tls_backend_conn_handshake(tls_backend_conn_t* c) {
    ERR_clear_error();
    int ret = SSL_do_handshake(c->ssl);
    return _tlsb_state(c->ssl, ret);
}

tls_backend_state_t tls_backend_conn_read(
    tls_backend_conn_t* c,
    void*               buf,
    int                 len,
    int*                out_n) {
    /**
     * No ERR_clear_error here: _tlsb_state drains the queue on the error
     * path, so it is already empty on entry (see _tlsb_state). Skipping the
     * per-call clear is a large win on small-record workloads.
     */
    int n = SSL_read(c->ssl, buf, len);
    if (n > 0) {
        *out_n = n;
        return TLS_BACKEND_OK;
    }
    *out_n = 0;
    return _tlsb_state(c->ssl, n);
}

tls_backend_state_t tls_backend_conn_write(
    tls_backend_conn_t* c,
    const void*         buf,
    int                 len,
    int*                out_n) {
    /**
     * See tls_backend_conn_read: the error queue is kept empty by the error
     * path, so the hot write path skips the per-call ERR_clear_error.
     */
    int n = SSL_write(c->ssl, buf, len);
    if (n > 0) {
        *out_n = n;
        return TLS_BACKEND_OK;
    }
    *out_n = 0;
    return _tlsb_state(c->ssl, n);
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

void dtls_backend_conn_set_mtu(tls_backend_conn_t* c, uint16_t mtu) {
    if (mtu == 0) {
        return;
    }
    c->mtu = mtu;
    SSL_set_options(c->ssl, SSL_OP_NO_QUERY_MTU);
    DTLS_set_link_mtu(c->ssl, mtu);
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
