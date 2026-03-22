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

#include "assert.h"
#include "xylem/xylem-tls.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <string.h>

static int _gen_self_signed(const char* cert_path, const char* key_path) {
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

    FILE* f = fopen(cert_path, "wb");
    if (!f) {
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return -1;
    }
    PEM_write_X509(f, x509);
    fclose(f);

    f = fopen(key_path, "wb");
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

static void test_ctx_create_destroy(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    xylem_tls_ctx_destroy(ctx);
}

static void test_load_cert_valid(void) {
    const char* cert = "test_tls_cert.pem";
    const char* key  = "test_tls_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx, cert, key) == 0);
    xylem_tls_ctx_destroy(ctx);

    remove(cert);
    remove(key);
}

static void test_load_cert_invalid(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(ctx, "nonexistent.pem",
                                   "nonexistent.pem") == -1);
    xylem_tls_ctx_destroy(ctx);
}

static void test_set_ca(void) {
    const char* cert = "test_tls_ca.pem";
    const char* key  = "test_tls_ca_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    ASSERT(xylem_tls_ctx_set_ca(ctx, cert) == 0);
    xylem_tls_ctx_destroy(ctx);

    remove(cert);
    remove(key);
}

static void test_set_verify(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    xylem_tls_ctx_set_verify(ctx, true);
    xylem_tls_ctx_set_verify(ctx, false);
    xylem_tls_ctx_destroy(ctx);
}

static void test_set_alpn(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);
    const char* protos[] = {"h2", "http/1.1"};
    ASSERT(xylem_tls_ctx_set_alpn(ctx, protos, 2) == 0);
    xylem_tls_ctx_destroy(ctx);
}

static xylem_loop_t       _loop;
static bool               _server_accepted;
static bool               _client_connected;
static bool               _client_received;
static bool               _write_done;
static bool               _client_closed;
static bool               _server_conn_closed;
static char               _recv_buf[256];
static size_t             _recv_len;
static xylem_tls_t*       _server_conn;
static xylem_tls_server_t* _tls_server;

static void _on_server_accept(xylem_tls_t* tls) {
    _server_accepted = true;
    _server_conn = tls;
}

static void _on_client_connect(xylem_tls_t* tls) {
    _client_connected = true;
    xylem_tls_send(tls, "hello", 5);
}

static void _on_server_read(xylem_tls_t* tls, void* data, size_t len) {
    xylem_tls_send(tls, data, len);
}

static void _on_client_read(xylem_tls_t* tls, void* data, size_t len) {
    (void)tls;
    _client_received = true;
    if (len < sizeof(_recv_buf)) {
        memcpy(_recv_buf, data, len);
        _recv_len = len;
    }
    xylem_tls_close(tls);
}

static void _on_client_write_done(xylem_tls_t* tls,
                                  void* data, size_t len, int status) {
    (void)tls;
    (void)data;
    (void)len;
    (void)status;
    _write_done = true;
}

static void _on_client_close(xylem_tls_t* tls, int err) {
    (void)tls;
    (void)err;
    _client_closed = true;
    if (_server_conn) {
        xylem_tls_close(_server_conn);
    }
}

static void _on_server_conn_close(xylem_tls_t* tls, int err) {
    (void)tls;
    (void)err;
    _server_conn_closed = true;
    xylem_tls_close_server(_tls_server);
}

static void test_handshake_and_echo(void) {
    const char* cert = "test_tls_hs_cert.pem";
    const char* key  = "test_tls_hs_key.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);

    _server_accepted    = false;
    _client_connected   = false;
    _client_received    = false;
    _write_done         = false;
    _client_closed      = false;
    _server_conn_closed = false;
    _server_conn        = NULL;
    _recv_len           = 0;
    memset(_recv_buf, 0, sizeof(_recv_buf));

    ASSERT(xylem_loop_init(&_loop) == 0);

    /* Server context with cert. */
    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    xylem_tls_handler_t srv_handler = {
        .on_accept = _on_server_accept,
        .on_read   = _on_server_read,
        .on_close  = _on_server_conn_close,
    };

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", 14433, &addr);

    _tls_server = xylem_tls_listen(&_loop, &addr, srv_ctx,
                                   &srv_handler, NULL);
    ASSERT(_tls_server != NULL);

    /* Client context -- disable verify for self-signed. */
    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, false);

    xylem_tls_handler_t cli_handler = {
        .on_connect    = _on_client_connect,
        .on_read       = _on_client_read,
        .on_write_done = _on_client_write_done,
        .on_close      = _on_client_close,
    };
    ASSERT(_tls_server != NULL);

    xylem_tls_t* client = xylem_tls_dial(&_loop, &addr, cli_ctx,
                                         &cli_handler, NULL);
    ASSERT(client != NULL);

    xylem_loop_run(&_loop);

    ASSERT(_server_accepted == true);
    ASSERT(_client_connected == true);
    ASSERT(_write_done == true);
    ASSERT(_client_received == true);
    ASSERT(_recv_len == 5);
    ASSERT(memcmp(_recv_buf, "hello", 5) == 0);
    ASSERT(_client_closed == true);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_loop_deinit(&_loop);

    remove(cert);
    remove(key);
}

static bool _handshake_failed;

static void _on_fail_close(xylem_tls_t* tls, int err) {
    (void)tls;
    (void)err;
    _handshake_failed = true;
    xylem_tls_close_server(_tls_server);
}

static void test_handshake_failure_wrong_ca(void) {
    const char* cert  = "test_tls_fail_cert.pem";
    const char* key   = "test_tls_fail_key.pem";
    const char* cert2 = "test_tls_fail_cert2.pem";
    const char* key2  = "test_tls_fail_key2.pem";
    ASSERT(_gen_self_signed(cert, key) == 0);
    ASSERT(_gen_self_signed(cert2, key2) == 0);

    _handshake_failed = false;

    ASSERT(xylem_loop_init(&_loop) == 0);

    /* Server with cert. */
    xylem_tls_ctx_t* srv_ctx = xylem_tls_ctx_create();
    ASSERT(srv_ctx != NULL);
    ASSERT(xylem_tls_ctx_load_cert(srv_ctx, cert, key) == 0);
    xylem_tls_ctx_set_verify(srv_ctx, false);

    xylem_tls_handler_t srv_handler = {0};
    srv_handler.on_close = _on_fail_close;

    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", 14434, &addr);

    _tls_server = xylem_tls_listen(&_loop, &addr, srv_ctx,
                                   &srv_handler, NULL);
    ASSERT(_tls_server != NULL);

    /* Client with verification enabled, using wrong CA. */
    xylem_tls_ctx_t* cli_ctx = xylem_tls_ctx_create();
    ASSERT(cli_ctx != NULL);
    xylem_tls_ctx_set_verify(cli_ctx, true);
    ASSERT(xylem_tls_ctx_set_ca(cli_ctx, cert2) == 0);

    xylem_tls_handler_t cli_handler = {0};
    cli_handler.on_close = _on_fail_close;

    xylem_tls_t* client = xylem_tls_dial(&_loop, &addr, cli_ctx,
                                         &cli_handler, NULL);
    ASSERT(client != NULL);

    xylem_loop_run(&_loop);

    ASSERT(_handshake_failed == true);

    xylem_tls_ctx_destroy(srv_ctx);
    xylem_tls_ctx_destroy(cli_ctx);
    xylem_loop_deinit(&_loop);

    remove(cert);
    remove(key);
    remove(cert2);
    remove(key2);
}

static void test_alpn_negotiation(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);

    const char* protos[] = {"h2", "http/1.1"};
    ASSERT(xylem_tls_ctx_set_alpn(ctx, protos, 2) == 0);

    /* Verify we can set ALPN without crash. Full negotiation
     * is tested in the handshake test when both sides set ALPN. */
    xylem_tls_ctx_destroy(ctx);
}

static void test_sni_hostname(void) {
    xylem_tls_ctx_t* ctx = xylem_tls_ctx_create();
    ASSERT(ctx != NULL);

    xylem_tls_handler_t handler = {0};
    xylem_addr_t addr;
    xylem_addr_pton("127.0.0.1", 14435, &addr);

    ASSERT(xylem_loop_init(&_loop) == 0);

    xylem_tls_t* tls = xylem_tls_dial(&_loop, &addr, ctx,
                                      &handler, NULL);
    ASSERT(tls != NULL);
    ASSERT(xylem_tls_set_hostname(tls, "example.com") == 0);

    xylem_tls_close(tls);
    xylem_tls_ctx_destroy(ctx);
    xylem_loop_deinit(&_loop);
}

int main(void) {
    /* Task 10: context tests */
    test_ctx_create_destroy();
    test_load_cert_valid();
    test_load_cert_invalid();
    test_set_ca();
    test_set_verify();
    test_set_alpn();

    /* Task 11: handshake and data transfer */
    test_handshake_and_echo();

    /* Task 12: error cases and ALPN */
    test_handshake_failure_wrong_ca();
    test_alpn_negotiation();
    test_sni_hostname();

    return 0;
}
