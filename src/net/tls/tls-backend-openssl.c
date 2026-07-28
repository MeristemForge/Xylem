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
#include <openssl/pemerr.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TLSB_COOKIE_SIZE        32
#define TLSB_DEFAULT_DGRAM_MTU  1500
#define TLSB_DNS_NAME_CAP       256

/**
 * The engine (tls.c) chunks plaintext writes to a backend-neutral 16 KiB
 * (TLS_MAX_PLAINTEXT) so the backend emits at most one record of
 * ciphertext per step. Assert that constant matches OpenSSL's own
 * per-record plaintext cap, so a chunk never spills into a second record.
 */
_Static_assert(SSL3_RT_MAX_PLAIN_LENGTH == 16 * 1024,
               "TLS_MAX_PLAINTEXT must match OpenSSL record plaintext cap");

typedef struct _tlsb_sni_entry_s {
    char            hostname[TLSB_DNS_NAME_CAP];
    X509*           cert;
    EVP_PKEY*       key;
    STACK_OF(X509)* chain;
} _tlsb_sni_entry_t;

struct tls_backend_ctx_s {
    SSL_CTX*            ssl_ctx;
    tls_backend_proto_t proto;
    uint8_t*            alpn_wire;
    size_t              alpn_wire_len;
    FILE*               keylog_file;
    _tlsb_sni_entry_t*  sni_entries;
    size_t              sni_count;
    size_t              sni_cap;
    uint8_t             cookie_secret[TLSB_COOKIE_SIZE]; /* DTLS only */
};

struct tls_backend_conn_s {
    SSL*                    ssl;
    tls_backend_io_t        io;
    uint16_t                mtu;
    bool                    fatal;
    struct sockaddr_storage peer;     /* DTLS server cookie binding */
    size_t                  peer_len;
};

typedef long (*_tlsb_bio_ctrl_fn_t)(
    BIO*  bio,
    int   cmd,
    long  num,
    void* ptr);

static int _tlsb_ctx_ex_idx  = -1;  /* SSL_CTX -> tls_backend_ctx_t* */
static int _tlsb_conn_ex_idx = -1;  /* SSL     -> tls_backend_conn_t* (DTLS) */
static once_flag _tlsb_ex_once = ONCE_FLAG_INIT;
static BIO_METHOD* _tlsb_stream_bio_method;
static BIO_METHOD* _tlsb_dgram_bio_method;

static int _tlsb_transport_bio_create(BIO* bio) {
    BIO_set_init(bio, 1);
    BIO_set_data(bio, NULL);
    return 1;
}

static int _tlsb_transport_bio_destroy(BIO* bio) {
    if (!bio) {
        return 0;
    }
    BIO_set_data(bio, NULL);
    BIO_set_init(bio, 0);
    return 1;
}

static int _tlsb_transport_bio_read(BIO* bio, char* out, int len) {
    tls_backend_conn_t* c = (tls_backend_conn_t*)BIO_get_data(bio);
    if (!c || !out || len <= 0) {
        return 0;
    }

    BIO_clear_retry_flags(bio);
    int n = c->io.read(c->io.user, out, len);
    if (n == TLS_BACKEND_IO_AGAIN) {
        BIO_set_retry_read(bio);
        return -1;
    }
    return n;
}

static int _tlsb_transport_bio_write(BIO* bio, const char* in, int len) {
    tls_backend_conn_t* c = (tls_backend_conn_t*)BIO_get_data(bio);
    if (!c || !in || len <= 0) {
        return 0;
    }

    BIO_clear_retry_flags(bio);
    int n = c->io.write(c->io.user, in, len);
    if (n == TLS_BACKEND_IO_AGAIN) {
        BIO_set_retry_write(bio);
        return -1;
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

static BIO_METHOD* _tlsb_bio_method_create(
    int                  type,
    const char*          name,
    _tlsb_bio_ctrl_fn_t  ctrl) {
    BIO_METHOD* method = BIO_meth_new(type, name);
    if (!method) {
        return NULL;
    }
    BIO_meth_set_create(method, _tlsb_transport_bio_create);
    BIO_meth_set_destroy(method, _tlsb_transport_bio_destroy);
    BIO_meth_set_read(method, _tlsb_transport_bio_read);
    BIO_meth_set_write(method, _tlsb_transport_bio_write);
    BIO_meth_set_ctrl(method, ctrl);
    return method;
}

static void _tlsb_init_ex(void) {
    _tlsb_ctx_ex_idx  = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
    _tlsb_conn_ex_idx = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
    _tlsb_stream_bio_method = _tlsb_bio_method_create(
        BIO_TYPE_SOURCE_SINK,
        "xylem-stream",
        _tlsb_stream_bio_ctrl);
    _tlsb_dgram_bio_method = _tlsb_bio_method_create(
        BIO_TYPE_DGRAM,
        "xylem-dgram",
        _tlsb_dgram_bio_ctrl);
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
        /* Remove inherited key-type slots before selecting this identity. */
        SSL_certs_clear(ssl);
        if (SSL_use_certificate(ssl, e->cert) != 1
            || SSL_use_PrivateKey(ssl, e->key) != 1) {
            xylem_loge("<tls> sni apply cert failed host=%s", e->hostname);
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        int chain_rc = e->chain ? SSL_set1_chain(ssl, e->chain)
                                : SSL_clear_chain_certs(ssl);
        if (chain_rc != 1) {
            xylem_loge("<tls> sni apply cert failed host=%s", e->hostname);
            return SSL_TLSEXT_ERR_ALERT_FATAL;
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

static int _tlsb_pem_password_cb(
    char* buf,
    int   size,
    int   rwflag,
    void* user) {
    (void)buf;
    (void)size;
    (void)rwflag;
    (void)user;
    return -1;
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
        ERR_clear_error();
        X509* extra = PEM_read_bio_X509(cbio, NULL, NULL, NULL);
        if (!extra) {
            /* NULL also reports parse errors; only NO_START_LINE ends the chain. */
            unsigned long err = ERR_peek_last_error();
            if (ERR_GET_LIB(err) == ERR_LIB_PEM
                && ERR_GET_REASON(err) == PEM_R_NO_START_LINE) {
                ERR_clear_error();
                break;
            }
            xylem_loge("<tls> parse certificate chain failed");
            ERR_clear_error();
            sk_X509_pop_free(chain, X509_free);
            X509_free(leaf);
            return -1;
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

    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(
        kbio, NULL, _tlsb_pem_password_cb, NULL);
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
    if (cert_len > INT_MAX || key_len > INT_MAX) {
        return -1;
    }
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

static int _tlsb_normalize_dns_name(
    const char*  name,
    char*        buf,
    size_t       cap,
    const char** out) {
    *out = name;
    if (!name) {
        return 0;
    }

    size_t len = strlen(name);
    if (len <= 1 || name[len - 1] != '.') {
        return 0;
    }
    if (len > cap) {
        return -1;
    }

    /* SNI and certificate matching omit the absolute name's root label. */
    memcpy(buf, name, len - 1);
    buf[len - 1] = '\0';
    *out         = buf;
    return 0;
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
    for (size_t i = 0; i < ctx->sni_count; i++) {
        _tlsb_sni_entry_t* entry = &ctx->sni_entries[i];
        if (platform_strcasecmp(hostname, entry->hostname) != 0) {
            continue;
        }
        X509_free(entry->cert);
        EVP_PKEY_free(entry->key);
        sk_X509_pop_free(entry->chain, X509_free);
        snprintf(entry->hostname, sizeof(entry->hostname), "%s", hostname);
        entry->cert  = leaf;
        entry->key   = key;
        entry->chain = chain;
        return 0;
    }

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
    X509* current = SSL_CTX_get0_certificate(ctx->ssl_ctx);
    if (current) {
        EVP_PKEY* current_key = X509_get0_pubkey(current);
        int       key_type    = EVP_PKEY_get_base_id(key);
        if (!current_key || key_type == EVP_PKEY_NONE
            || EVP_PKEY_get_base_id(current_key) != key_type) {
            return -1;
        }
    }

    if (SSL_CTX_use_certificate(ctx->ssl_ctx, leaf) != 1
        || SSL_CTX_use_PrivateKey(ctx->ssl_ctx, key) != 1) {
        return -1;
    }
    int chain_rc = chain ? SSL_CTX_set1_chain(ctx->ssl_ctx, chain)
                         : SSL_CTX_clear_chain_certs(ctx->ssl_ctx);
    if (chain_rc != 1) {
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
        char        hostname_buf[TLSB_DNS_NAME_CAP];
        const char* normalized_hostname;
        if (_tlsb_normalize_dns_name(hostname,
                                     hostname_buf,
                                     sizeof(hostname_buf),
                                     &normalized_hostname)
            == 0) {
            return _tlsb_store_sni_identity(
                ctx, normalized_hostname, leaf, key, chain);
        }
        EVP_PKEY_free(key);
        sk_X509_pop_free(chain, X509_free);
        X509_free(leaf);
        return -1;
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
        SSL_CTX_set_options(ctx->ssl_ctx, SSL_OP_COOKIE_EXCHANGE);
        SSL_CTX_set_min_proto_version(ctx->ssl_ctx, DTLS1_2_VERSION);
    } else {
        /* Avoid separate transport reads for the TLS record header and body. */
        SSL_CTX_set_read_ahead(ctx->ssl_ctx, 1);
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
    if (!protocols || count == 0) {
        return -1;
    }

    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (!protocols[i]) {
            return -1;
        }
        size_t plen = strlen(protocols[i]);
        if (plen == 0 || plen > UINT8_MAX
            || total > UINT_MAX - 1 - plen) {
            return -1;
        }
        total += 1 + plen;
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

    /**
     * One list serves both roles: the client offers it, the server uses
     * the select cb to pick from it. The unused half is inert per role.
     */
    if (SSL_CTX_set_alpn_protos(ctx->ssl_ctx, wire, (unsigned int)total) != 0) {
        free(wire);
        return -1;
    }

    free(ctx->alpn_wire);
    ctx->alpn_wire     = wire;
    ctx->alpn_wire_len = total;
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
    SSL_CTX_set_keylog_callback(ctx->ssl_ctx, _tlsb_ssl_keylog_cb);
    return 0;
}

tls_backend_conn_t* tls_backend_conn_create(
    tls_backend_ctx_t* ctx,
    bool               is_server,
    const tls_backend_io_t* io) {
    if (!ctx || !io || !io->read || !io->write) {
        return NULL;
    }

    tls_backend_conn_t* c = (tls_backend_conn_t*)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->ssl = SSL_new(ctx->ssl_ctx);
    if (!c->ssl) {
        free(c);
        return NULL;
    }

    BIO_METHOD* method = (ctx->proto == TLS_BACKEND_PROTO_DTLS)
        ? _tlsb_dgram_bio_method
        : _tlsb_stream_bio_method;
    if (!method) {
        SSL_free(c->ssl);
        free(c);
        return NULL;
    }

    c->io    = *io;
    BIO* bio = BIO_new(method);
    if (!bio) {
        SSL_free(c->ssl);
        free(c);
        return NULL;
    }
    BIO_set_data(bio, c);
    /* With the same new BIO for read and write, SSL_set_bio takes one ref. */
    SSL_set_bio(c->ssl, bio, bio);

    /* DTLS server cookie path needs SSL -> conn lookup. */
    SSL_set_ex_data(c->ssl, _tlsb_conn_ex_idx, c);

    /* Select the role before SSL_do_handshake() drives the state machine. */
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

int tls_backend_conn_configure(
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

    char        sni_buf[TLSB_DNS_NAME_CAP];
    const char* sni_name;
    if (_tlsb_normalize_dns_name(
            cfg->sni_name, sni_buf, sizeof(sni_buf), &sni_name)
        != 0) {
        return -1;
    }
    if (sni_name && SSL_set_tlsext_host_name(c->ssl, sni_name) != 1) {
        return -1;
    }

    char        verify_buf[TLSB_DNS_NAME_CAP];
    const char* verify_dns_name;
    if (_tlsb_normalize_dns_name(cfg->verify_dns_name,
                                 verify_buf,
                                 sizeof(verify_buf),
                                 &verify_dns_name)
        != 0) {
        return -1;
    }
    if (verify_dns_name && SSL_set1_host(c->ssl, verify_dns_name) != 1) {
        return -1;
    }
    if (cfg->verify_ip_address) {
        X509_VERIFY_PARAM* param = SSL_get0_param(c->ssl);
        if (!param
            || X509_VERIFY_PARAM_set1_ip_asc(
                   param,
                   cfg->verify_ip_address)
                   != 1) {
            return -1;
        }
    }
    return 0;
}

static void _tlsb_clear_error(void) {
    if (ERR_peek_error() != 0) {
        ERR_clear_error();
    }
}

static tls_backend_state_t _tlsb_state(tls_backend_conn_t* c, int ret) {
    /**
     * Handshake success: SSL_do_handshake returns 1 (read/write map
     * their own >0 before this). Else ret==1 -> SSL_ERROR_NONE -> false
     * error.
     */
    if (ret == 1) {
        return TLS_BACKEND_OK;
    }
    int err = SSL_get_error(c->ssl, ret);
    switch (err) {
        case SSL_ERROR_WANT_READ:   return TLS_BACKEND_WANT_READ;
        case SSL_ERROR_WANT_WRITE:  return TLS_BACKEND_WANT_WRITE;
        case SSL_ERROR_ZERO_RETURN: return TLS_BACKEND_CLOSED;
        case SSL_ERROR_SSL:
        case SSL_ERROR_SYSCALL: c->fatal = true; break;
        default: break;
    }
    unsigned long e = ERR_peek_error();
    xylem_loge("<tls> ssl op failed ssl_err=%d reason=%s", err,
               ERR_reason_error_string(e) ? ERR_reason_error_string(e)
                                          : "unknown");
    /**
     * Drain the queue on the error path so it is empty again for the next
     * op. Read/write still clear before entry because the thread-local queue
     * may contain errors from unrelated OpenSSL calls on the same thread.
     */
    ERR_clear_error();
    return TLS_BACKEND_ERROR;
}

tls_backend_state_t tls_backend_conn_handshake(tls_backend_conn_t* c) {
    _tlsb_clear_error();
    int ret = SSL_do_handshake(c->ssl);
    return _tlsb_state(c, ret);
}

tls_backend_state_t tls_backend_conn_read(
    tls_backend_conn_t* c,
    void*               buf,
    int                 len,
    int*                out_n) {
    _tlsb_clear_error();
    int n = SSL_read(c->ssl, buf, len);
    if (n > 0) {
        *out_n = n;
        return TLS_BACKEND_OK;
    }
    *out_n = 0;
    return _tlsb_state(c, n);
}

tls_backend_state_t tls_backend_conn_write(
    tls_backend_conn_t* c,
    const void*         buf,
    int                 len,
    int*                out_n) {
    _tlsb_clear_error();
    int n = SSL_write(c->ssl, buf, len);
    if (n > 0) {
        *out_n = n;
        return TLS_BACKEND_OK;
    }
    *out_n = 0;
    return _tlsb_state(c, n);
}

void tls_backend_conn_shutdown(tls_backend_conn_t* c) {
    if (c->ssl && !c->fatal) {
        ERR_clear_error();
        (void)SSL_shutdown(c->ssl);   /* best-effort close_notify */
        ERR_clear_error();
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
