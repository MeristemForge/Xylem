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

#include "net/addr.h"
#include "platform/platform-io.h"
#include "platform/platform-socket.h"
#include "platform/platform-string.h"
#include "thrds.h"

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

#define TLSB_COOKIE_SIZE 32

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
    SSL*  ssl;
    BIO*  rbio;   /* inbound ciphertext: feed() -> SSL */
    BIO*  wbio;   /* outbound ciphertext: SSL -> drain() */
    struct sockaddr_storage peer;     /* DTLS server cookie binding */
    size_t                  peer_len;
};

static int _tlsb_ctx_ex_idx  = -1;  /* SSL_CTX -> tls_backend_ctx_t* */
static int _tlsb_conn_ex_idx = -1;  /* SSL     -> tls_backend_conn_t* (DTLS) */
static once_flag _tlsb_ex_once = ONCE_FLAG_INIT;

static void _tlsb_init_ex(void) {
    _tlsb_ctx_ex_idx  = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
    _tlsb_conn_ex_idx = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

/**
 * SNI servername callback. Selects the per-host certificate by setting
 * it directly on the SSL connection -- the single ctx (and thus its
 * keylog / ALPN / verify config) stays in force. Mirrors Go's
 * GetCertificate / rustls' cert resolver: SNI only picks a cert, never
 * swaps the whole configuration. When no host matches, the ctx default
 * certificate is left untouched.
 */
static int _tlsb_sni_cb(SSL* ssl, int* al, void* arg) {
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

static void _tlsb_keylog_cb(const SSL* ssl, const char* line) {
    SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
    tls_backend_ctx_t* ctx =
        (tls_backend_ctx_t*)SSL_CTX_get_ex_data(ssl_ctx, _tlsb_ctx_ex_idx);
    if (ctx && ctx->keylog_file) {
        fprintf(ctx->keylog_file, "%s\n", line);
        fflush(ctx->keylog_file);
    }
}

static int _tlsb_alpn_select_cb(SSL* ssl, const unsigned char** out,
                                unsigned char* outlen,
                                const unsigned char* in,
                                unsigned int inlen, void* arg) {
    tls_backend_ctx_t* ctx = (tls_backend_ctx_t*)arg;
    (void)ssl;

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
static int _tlsb_parse_pem_identity(BIO* cbio, BIO* kbio,
                                    X509** out_cert, EVP_PKEY** out_key,
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
static int _tlsb_load_pem_identity(const char* cert_file,
                                   const char* key_file,
                                   X509** out_cert,
                                   EVP_PKEY** out_key,
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
static int _tlsb_load_pem_identity_mem(const void* cert_pem, size_t cert_len,
                                       const void* key_pem, size_t key_len,
                                       X509** out_cert, EVP_PKEY** out_key,
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
static int _tlsb_store_sni_identity(tls_backend_ctx_t* ctx,
                                    const char* hostname,
                                    X509* leaf, EVP_PKEY* key,
                                    STACK_OF(X509)* chain) {
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
static int _tlsb_apply_default_identity(tls_backend_ctx_t* ctx, X509* leaf,
                                        EVP_PKEY* key, STACK_OF(X509)* chain) {
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
static int _tlsb_install_identity(tls_backend_ctx_t* ctx, const char* hostname,
                                  X509* leaf, EVP_PKEY* key,
                                  STACK_OF(X509)* chain) {
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

static int _tlsb_cookie_generate_cb(SSL* ssl, unsigned char* cookie,
                                    unsigned int* cookie_len) {
    SSL_CTX* sc = SSL_get_SSL_CTX(ssl);
    tls_backend_ctx_t* ctx =
        (tls_backend_ctx_t*)SSL_CTX_get_ex_data(sc, _tlsb_ctx_ex_idx);
    const uint8_t* msg; size_t msg_len;
    if (!ctx || _tlsb_cookie_peer(ssl, &msg, &msg_len) < 0) {
        return 0;
    }
    xylem_hmac256_compute(ctx->cookie_secret, sizeof(ctx->cookie_secret),
                          msg, msg_len, cookie);
    *cookie_len = TLSB_COOKIE_SIZE;
    return 1;
}

static int _tlsb_cookie_verify_cb(SSL* ssl, const unsigned char* cookie,
                                  unsigned int cookie_len) {
    SSL_CTX* sc = SSL_get_SSL_CTX(ssl);
    tls_backend_ctx_t* ctx =
        (tls_backend_ctx_t*)SSL_CTX_get_ex_data(sc, _tlsb_ctx_ex_idx);
    const uint8_t* msg; size_t msg_len;
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

    SSL_CTX_set_tlsext_servername_callback(ctx->ssl_ctx, _tlsb_sni_cb);
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

int tls_backend_ctx_load_cert_file(tls_backend_ctx_t* ctx,
                                   const char* hostname,
                                   const char* cert_file,
                                   const char* key_file) {
    X509*           leaf  = NULL;
    EVP_PKEY*       pkey  = NULL;
    STACK_OF(X509)* chain = NULL;
    if (_tlsb_load_pem_identity(cert_file, key_file, &leaf, &pkey, &chain)
        != 0) {
        return -1;
    }
    return _tlsb_install_identity(ctx, hostname, leaf, pkey, chain);
}

int tls_backend_ctx_load_cert_mem(tls_backend_ctx_t* ctx,
                                  const char* hostname,
                                  const void* cert_pem, size_t cert_len,
                                  const void* key_pem,  size_t key_len) {
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

int tls_backend_ctx_load_system_ca(tls_backend_ctx_t* ctx) {
#if defined(_WIN32)
    if (SSL_CTX_load_verify_store(ctx->ssl_ctx,
                                  "org.openssl.winstore://") != 1) {
        xylem_loge("<tls> load system ca failed");
        return -1;
    }
#else
    if (SSL_CTX_set_default_verify_paths(ctx->ssl_ctx) != 1) {
        xylem_loge("<tls> load system ca failed");
        return -1;
    }
#endif
    return 0;
}

int tls_backend_ctx_set_alpn(tls_backend_ctx_t* ctx,
                             const char** protocols, size_t count) {
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
    SSL_CTX_set_keylog_callback(ctx->ssl_ctx, _tlsb_keylog_cb);
    return 0;
}
