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
 * HTTPS Static File Server
 *
 * Same as http-static-server but served over TLS.
 * If cert.pem / key.pem are missing, generates a self-signed
 * certificate automatically using the OpenSSL C API.
 *
 *   GET /static/(wildcard)  -> files from ./public/
 *
 * Usage: https-static-server
 * Test:  curl -k https://127.0.0.1:8443/static/index.html
 */

#include "xylem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#define LISTEN_PORT 8443
#define STATIC_ROOT "public"
#define CERT_FILE   "cert.pem"
#define KEY_FILE    "key.pem"

static xylem_http_router_t* _router;

/**
 * Write PEM data to a file via memory BIO instead of passing FILE* directly
 * to OpenSSL (e.g. PEM_write_X509). On Windows, the OpenSSL DLL and the
 * application may link against different C runtimes whose FILE structs are
 * incompatible. Passing a FILE* across the DLL boundary triggers the
 * OPENSSL_Applink error. Using a memory BIO keeps all FILE* operations
 * inside the application's own CRT, avoiding the issue entirely.
 */
static int _write_pem_to_file(const char* path,
                              int (*write_fn)(BIO*, void*),
                              void* obj) {
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
    return PEM_write_bio_PrivateKey(bio, (EVP_PKEY*)obj,
                                    NULL, NULL, 0, NULL, NULL);
}

static int _ensure_cert(void) {
    FILE* f = fopen(CERT_FILE, "r");
    if (f) {
        fclose(f);
        return 0;
    }

    xylem_logi("generating self-signed certificate...");

    EVP_PKEY* pkey = EVP_PKEY_new();
    if (!pkey) {
        return -1;
    }

    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    if (!pctx) {
        EVP_PKEY_free(pkey);
        return -1;
    }

    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048);
    EVP_PKEY_keygen(pctx, &pkey);
    EVP_PKEY_CTX_free(pctx);

    X509* x509 = X509_new();
    X509_set_version(x509, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 365 * 24 * 3600);
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char*)"localhost", -1, -1, 0);
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

/* Dispatches requests through the router. */
static void _on_request(xylem_http_writer_t* w, xylem_http_req_t* req,
                         void* ud) {
    (void)ud;
    xylem_http_router_dispatch(_router, w, req);
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    if (_ensure_cert() != 0) {
        xylem_loge("failed to generate self-signed certificate");
        return 1;
    }

    xylem_loop_t* loop = xylem_loop_create();

    _router = xylem_http_router_create();

    xylem_http_static_opts_t opts = {
        .root          = STATIC_ROOT,
        .index_file    = "index.html",
        .max_age       = 3600,
        .precompressed = true,
    };
    xylem_http_static_serve(_router, "/static", &opts);

    xylem_http_srv_cfg_t cfg = {
        .host       = "127.0.0.1",
        .port       = LISTEN_PORT,
        .on_request = _on_request,
        .userdata   = NULL,
        .tls_cert   = CERT_FILE,
        .tls_key    = KEY_FILE,
    };

    xylem_http_srv_t* srv = xylem_http_listen(loop, &cfg);
    if (!srv) {
        xylem_loge("failed to start https server on port %d", LISTEN_PORT);
        return 1;
    }

    xylem_logi("serving %s/ at https://127.0.0.1:%d/static/",
               STATIC_ROOT, LISTEN_PORT);
    xylem_loop_run(loop);

    xylem_http_close_server(srv);
    xylem_http_router_destroy(_router);
    xylem_loop_destroy(loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
