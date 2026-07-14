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
 * Xylem TLS Echo Benchmark Server (single-threaded)
 *
 * One acceptor coroutine loops xylem_tls_accept() (TCP accept + TLS handshake
 * handled internally) and spawns a handler coroutine per connection that
 * echoes plaintext back. A self-signed ECDSA P-256 certificate is generated
 * at startup via OpenSSL. Runs under a single scheduler worker (ST).
 *
 * Usage: tls-xylem-echo [port]
 */

#include "xylem.h"
#include "xylem/net/xylem-tls.h"

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define DEFAULT_PORT  9443
#define READ_BUF_SIZE 65536
#define CERT_FILE     "bench-cert.pem"
#define KEY_FILE      "bench-key.pem"

/* Parse a base-10 integer in [min, max]; returns fallback on any error. */
static long _parse_int(const char* s, long min, long max, long fallback) {
    char* end = NULL;
    long  v   = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < min || v > max) {
        return fallback;
    }
    return v;
}

static size_t _read_buf_size(void) {
    const char* s = getenv("XYLEM_TLS_ECHO_READ_BUF");
    if (!s) {
        return READ_BUF_SIZE;
    }
    return (size_t)_parse_int(s, 1024, READ_BUF_SIZE, READ_BUF_SIZE);
}

static int
_write_pem_to_file(const char* path, int (*write_fn)(BIO*, void*), void* obj) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) {
        return -1;
    }
    if (write_fn(bio, obj) != 1) {
        BIO_free(bio);
        return -1;
    }

    char* data = NULL;
    long  len  = BIO_get_mem_data(bio, &data);
    FILE* f    = fopen(path, "wb");
    if (!f) {
        BIO_free(bio);
        return -1;
    }
    fwrite(data, 1, (size_t)len, f);
    fclose(f);
    BIO_free(bio);
    return 0;
}

static int _write_cert(BIO* bio, void* obj) {
    return PEM_write_bio_X509(bio, (X509*)obj);
}

static int _write_key(BIO* bio, void* obj) {
    return PEM_write_bio_PrivateKey(
        bio,
        (EVP_PKEY*)obj,
        NULL,
        NULL,
        0,
        NULL,
        NULL);
}

static int _ensure_cert(void) {
    FILE* f = fopen(CERT_FILE, "r");
    if (f) {
        fclose(f);
        return 0;
    }

    fprintf(stderr, "generating self-signed certificate...\n");

    /* ECDSA P-256 to match the go/rust bench servers (rcgen / ecdsa default),
     * so connrate compares like-for-like server signing cost instead of
     * RSA-2048 vs EC. */
    EVP_PKEY*     pkey = NULL;
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (!pctx) {
        return -1;
    }

    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1);
    EVP_PKEY_keygen(pctx, &pkey);
    EVP_PKEY_CTX_free(pctx);
    if (!pkey) {
        return -1;
    }

    X509* x509 = X509_new();
    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 365 * 24 * 3600);
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(
        name,
        "CN",
        MBSTRING_ASC,
        (const unsigned char*)"localhost",
        -1,
        -1,
        0);
    X509_set_issuer_name(x509, name);
    X509_sign(x509, pkey, EVP_sha256());

    int rc = 0;
    if (_write_pem_to_file(CERT_FILE, _write_cert, x509) != 0) {
        rc = -1;
    }
    if (rc == 0 && _write_pem_to_file(KEY_FILE, _write_key, pkey) != 0) {
        rc = -1;
    }

    X509_free(x509);
    EVP_PKEY_free(pkey);
    return rc;
}

static void _handle_conn(void* arg) {
    xylem_tls_conn_t* conn = (xylem_tls_conn_t*)arg;
    size_t            buf_size = _read_buf_size();

    /* Heap buffer: 64 KiB would consume half the 128 KiB coroutine stack. */
    char* buf = (char*)malloc(buf_size);
    if (!buf) {
        xylem_tls_destroy(conn);
        return;
    }

    for (;;) {
        int n = xylem_tls_read(conn, buf, (int)buf_size);
        if (n <= 0) {
            break;
        }
        if (xylem_tls_write(conn, buf, n) != 0) {
            break;
        }
    }

    free(buf);
    xylem_tls_destroy(conn);
}

static void _acceptor(void* arg) {
    uint16_t port = *(uint16_t*)arg;

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    if (!ctx) {
        fprintf(stderr, "tls echo: ctx create failed\n");
        xylem_shutdown();
        return;
    }

    if (_ensure_cert() != 0) {
        fprintf(stderr, "tls echo: certificate generation failed\n");
        xylem_tls_ctx_destroy(ctx);
        xylem_shutdown();
        return;
    }

    if (xylem_tls_ctx_load_cert(ctx, NULL, CERT_FILE, KEY_FILE) != 0) {
        fprintf(
            stderr,
            "tls echo: load cert failed %s / %s\n",
            CERT_FILE,
            KEY_FILE);
        xylem_tls_ctx_destroy(ctx);
        xylem_shutdown();
        return;
    }
    xylem_tls_ctx_verify_client(ctx, false);

    xylem_tls_opts_t opts = {0}; /* default: MSS unclamped, use path MTU */
    xylem_tls_listener_t* server =
        xylem_tls_listen("0.0.0.0", port, ctx, &opts);
    if (!server) {
        fprintf(stderr, "tls echo: listen failed port=%" PRIu16 "\n", port);
        xylem_tls_ctx_destroy(ctx);
        xylem_shutdown();
        return;
    }

    fprintf(
        stderr,
        "xylem tls echo server listening on 0.0.0.0:%" PRIu16 "\n",
        port);

    for (;;) {
        xylem_tls_conn_t* conn = xylem_tls_accept(server);
        if (!conn) {
            break;
        }
        xylem_spawn(_handle_conn, conn);
    }

    xylem_tls_destroy_listener(server);
    xylem_tls_ctx_destroy(ctx);
}

int main(int argc, char** argv) {
    uint16_t port = DEFAULT_PORT;
    if (argc > 1) {
        port = (uint16_t)_parse_int(argv[1], 1, 65535, DEFAULT_PORT);
    }

    xylem_opts_t rt_opts = {.workers = 1};
    /* Experiment knob: override coroutine stack size (bytes) to probe the
     * per-connection memory / cache-footprint effect at high conn counts. */
    const char* cs = getenv("XYLEM_CORO_STACK");
    if (cs) {
        rt_opts.coro_stack_size = (size_t)strtoul(cs, NULL, 10);
    }
    xylem_run(_acceptor, &port, &rt_opts);
    return 0;
}
