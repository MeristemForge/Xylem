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
 * TLS Echo Server
 *
 * Listens on 127.0.0.1:9443 with TLS and echoes back every message.
 * If cert.pem / key.pem are missing, generates a self-signed certificate
 * automatically using the OpenSSL C API.
 *
 * Usage: tls-echo-server
 * Pair:  tls-echo-client
 */

#include "xylem.h"
#include "xylem/xylem-tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#define LISTEN_PORT 9443
#define CERT_FILE   "cert.pem"
#define KEY_FILE    "key.pem"

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

static void _on_accept(xylem_tls_server_t* server,
                       xylem_tls_conn_t* tls) {
    (void)server;
    (void)tls;
    xylem_logi("tls client connected");
}

static void _on_read(xylem_tls_conn_t* tls, void* data, size_t len) {
    xylem_logi("recv %zu bytes: %.*s", len, (int)len, (char*)data);
    xylem_tls_send(tls, data, len);
}

static void _on_close(xylem_tls_conn_t* tls, int err, const char* errmsg) {
    (void)tls;
    (void)err;
    xylem_logi("tls client disconnected (%s)", errmsg);
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    xylem_loop_t* loop = xylem_loop_create();

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    if (!ctx) {
        xylem_loge("failed to create tls context");
        return 1;
    }

    if (_ensure_cert() != 0) {
        xylem_loge("failed to generate self-signed certificate");
        xylem_tls_ctx_destroy(ctx);
        return 1;
    }

    if (xylem_tls_ctx_load_cert(ctx, CERT_FILE, KEY_FILE) != 0) {
        xylem_loge("failed to load %s / %s", CERT_FILE, KEY_FILE);
        xylem_tls_ctx_destroy(ctx);
        return 1;
    }

    xylem_tls_ctx_set_verify(ctx, false);

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", LISTEN_PORT, &addr);

    xylem_tls_handler_t handler = {
        .on_accept = _on_accept,
        .on_read   = _on_read,
        .on_close  = _on_close,
    };

    xylem_tls_server_t* server = xylem_tls_listen(loop, &addr, ctx,
                                                  &handler, NULL);
    if (!server) {
        xylem_loge("failed to listen on port %d", LISTEN_PORT);
        xylem_tls_ctx_destroy(ctx);
        return 1;
    }

    xylem_logi("tls echo server listening on 127.0.0.1:%d", LISTEN_PORT);
    xylem_loop_run(loop);

    xylem_tls_ctx_destroy(ctx);
    xylem_loop_destroy(loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
