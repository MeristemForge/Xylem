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
 * HTTPS Echo Server
 *
 * Same routes as http-echo-server but served over TLS.
 * If cert.pem / key.pem are missing, generates a self-signed
 * certificate automatically via the openssl command-line tool.
 *
 *   Middleware: request logger, simple auth check
 *   GET  /              -> "Hello, Xylem! (HTTPS)"
 *   POST /echo          -> echoes the request body as JSON
 *   *    (no match)     -> 404 via router
 *
 * Usage: https-echo-server
 * Test:  https-echo-client  (or curl -k https://127.0.0.1:8443/)
 */

#include "xylem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#define LISTEN_PORT 8443
#define CERT_FILE   "cert.pem"
#define KEY_FILE    "key.pem"

static xylem_http_router_t* _router;

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

/* Log every request: method + url. */
static int _log_middleware(xylem_http_writer_t* w, xylem_http_req_t* req,
                           void* ud) {
    (void)w;
    (void)ud;
    xylem_logi("[req] %s %s", xylem_http_req_method(req),
               xylem_http_req_url(req));
    return 0;
}

/**
 * Reject requests without a valid Authorization header.
 * Accepts "Bearer xylem-demo-token" for demonstration purposes.
 */
static int _auth_middleware(xylem_http_writer_t* w, xylem_http_req_t* req,
                            void* ud) {
    (void)ud;
    const char* auth = xylem_http_req_header(req, "Authorization");
    if (auth && strcmp(auth, "Bearer xylem-demo-token") == 0) {
        return 0;
    }
    xylem_http_writer_set_status(w, 401);
    xylem_http_writer_set_header(w, "Content-Type", "application/json");
    const char* body = "{\"error\":\"unauthorized\"}";
    xylem_http_writer_write(w, body, strlen(body));
    return -1;
}

/* GET / */
static void _handle_index(xylem_http_writer_t* w, xylem_http_req_t* req,
                           void* ud) {
    (void)req;
    (void)ud;
    xylem_http_writer_set_header(w, "Content-Type", "text/plain");
    const char* body = "Hello, Xylem! (HTTPS)";
    xylem_http_writer_write(w, body, strlen(body));
}

/* POST /echo */
static void _handle_echo(xylem_http_writer_t* w, xylem_http_req_t* req,
                          void* ud) {
    (void)ud;
    const void* body     = xylem_http_req_body(req);
    size_t      body_len = xylem_http_req_body_len(req);

    char buf[4096];
    int n = snprintf(buf, sizeof(buf),
                     "{\"method\":\"%s\",\"body\":\"%.*s\"}",
                     xylem_http_req_method(req),
                     (int)body_len, (const char*)body);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        xylem_http_writer_set_status(w, 500);
        xylem_http_writer_set_header(w, "Content-Type", "text/plain");
        xylem_http_writer_write(w, "response too large", 18);
        return;
    }
    xylem_http_writer_set_header(w, "Content-Type", "application/json");
    xylem_http_writer_write(w, buf, (size_t)n);
}

static void _on_request(xylem_http_writer_t* w, xylem_http_req_t* req,
                         void* ud) {
    (void)ud;
    xylem_http_router_dispatch(_router, w, req);
}

int main(void) {
    xylem_startup();
    xylem_logger_init(NULL, XYLEM_LOGGER_LEVEL_INFO, false, 0);

    if (_ensure_cert() != 0) {
        xylem_loge("failed to generate certificate (is openssl installed?)");
        return 1;
    }

    xylem_loop_t* loop = xylem_loop_create();

    _router = xylem_http_router_create();

    /* Register middleware (runs in order before route handlers). */
    xylem_http_router_use(_router, _log_middleware, NULL);
    xylem_http_router_use(_router, _auth_middleware, NULL);

    xylem_http_router_add(_router, "GET",  "/",     _handle_index, NULL);
    xylem_http_router_add(_router, "POST", "/echo", _handle_echo,  NULL);

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

    xylem_logi("https server listening on https://127.0.0.1:%d", LISTEN_PORT);
    xylem_loop_run(loop);

    xylem_http_close_server(srv);
    xylem_http_router_destroy(_router);
    xylem_loop_destroy(loop);
    xylem_logger_deinit();
    xylem_cleanup();
    return 0;
}
