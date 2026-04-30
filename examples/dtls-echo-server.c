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
 * DTLS Echo Server
 *
 * Listens on 127.0.0.1:9444 with DTLS and echoes back every datagram.
 * If cert.pem / key.pem are missing, generates a self-signed certificate
 * automatically via the openssl command-line tool.
 *
 * Usage: dtls-echo-server
 * Pair:  dtls-echo-client
 */

#include "xylem.h"
#include "xylem/xylem-dtls.h"

#include <stdio.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#define LISTEN_PORT 9444
#define CERT_FILE   "cert.pem"
#define KEY_FILE    "key.pem"

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

    f = fopen(CERT_FILE, "wb");
    if (!f) {
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return -1;
    }
    PEM_write_X509(f, x509);
    fclose(f);

    f = fopen(KEY_FILE, "wb");
    if (!f) {
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return -1;
    }
    PEM_write_PrivateKey(f, pkey, NULL, NULL, 0, NULL, NULL);
    fclose(f);

    X509_free(x509);
    EVP_PKEY_free(pkey);
    return 0;
}

static void _on_accept(xylem_dtls_server_t* server, xylem_dtls_conn_t* dtls) {
    (void)server;
    (void)dtls;
    xylem_logi("dtls client connected");
}

static void _on_read(xylem_dtls_conn_t* dtls, void* data, size_t len) {
    xylem_logi("recv %zu bytes: %.*s", len, (int)len, (char*)data);
    xylem_dtls_send(dtls, data, len);
}

static void _on_close(xylem_dtls_conn_t* dtls, int err, const char* errmsg) {
    (void)dtls;
    (void)err;
    (void)errmsg;
    xylem_logi("dtls client disconnected");
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    xylem_loop_t* loop = xylem_loop_create();

    xylem_dtls_ctx_t* ctx = xylem_dtls_ctx_create();
    if (!ctx) {
        xylem_loge("failed to create dtls context");
        return 1;
    }

    if (_ensure_cert() != 0) {
        xylem_loge("failed to generate certificate (is openssl installed?)");
        xylem_dtls_ctx_destroy(ctx);
        return 1;
    }

    if (xylem_dtls_ctx_load_cert(ctx, CERT_FILE, KEY_FILE) != 0) {
        xylem_loge("failed to load %s / %s", CERT_FILE, KEY_FILE);
        xylem_dtls_ctx_destroy(ctx);
        return 1;
    }

    xylem_dtls_ctx_set_verify(ctx, false);

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", LISTEN_PORT, &addr);

    xylem_dtls_handler_t handler = {
        .on_accept = _on_accept,
        .on_read   = _on_read,
        .on_close  = _on_close,
    };

    xylem_dtls_server_t* server = xylem_dtls_listen(loop, &addr, ctx,
                                                    &handler);
    if (!server) {
        xylem_loge("failed to listen on port %d", LISTEN_PORT);
        xylem_dtls_ctx_destroy(ctx);
        return 1;
    }

    xylem_logi("dtls echo server listening on 127.0.0.1:%d", LISTEN_PORT);
    xylem_loop_run(loop);

    xylem_dtls_ctx_destroy(ctx);
    xylem_loop_destroy(loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
