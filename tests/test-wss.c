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

/*
 * WSS (WebSocket over TLS) integration tests. test-ws.c covers plain ws://
 * and test-tls.c covers the raw TLS engine; this file covers the wss path:
 * a TLS-backed WebSocket listener, wss:// scheme dispatch in xylem_ws_dial,
 * and frame exchange (text/binary/large/permessage-deflate) over TLS.
 */

#include "xylem.h"
#include "xylem/net/xylem-ws.h"
#include "assert.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Self-signed certificate generation (matches test-tls.c). */

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

static int _write_cert_pem(BIO* bio, void* obj) {
    return PEM_write_bio_X509(bio, (X509*)obj);
}

static int _write_key_pem(BIO* bio, void* obj) {
    return PEM_write_bio_PrivateKey(bio, (EVP_PKEY*)obj,
                                    NULL, NULL, 0, NULL, NULL);
}

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

    X509_EXTENSION* ext_san = X509V3_EXT_nconf_nid(
        NULL, NULL, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1");
    if (ext_san) {
        X509_add_ext(x509, ext_san, -1);
        X509_EXTENSION_free(ext_san);
    }

    X509_sign(x509, pkey, EVP_sha256());

    int rc = 0;
    if (_write_pem_to_file(cert_path, _write_cert_pem, x509) != 0) {
        rc = -1;
    }
    if (rc == 0 && _write_pem_to_file(key_path, _write_key_pem, pkey) != 0) {
        rc = -1;
    }

    X509_free(x509);
    EVP_PKEY_free(pkey);
    return rc;
}

/* Helpers. */

#define WSS_CERT "test_wss_cert.pem"
#define WSS_KEY  "test_wss_key.pem"

static const xylem_ws_tls_t _srv_tls = { .cert = WSS_CERT, .key = WSS_KEY };
static const xylem_ws_tls_t _cli_tls = { .skip_verify = true };

static void _srv_echo_handler(xylem_ws_conn_t* ws, void* ud) {
    (void)ud;
    xylem_ws_msg_t msg;
    while (xylem_ws_recv(ws, &msg) == 0) {
        xylem_ws_send(ws, msg.opcode, msg.data, msg.len);
        xylem_ws_msg_free(&msg);
    }
    xylem_ws_close(ws, 1000, NULL, 0);
}

/* Test: wss text echo. */

static void test_wss_text_echo(void* arg) {
    (void)arg;
    ASSERT(_gen_self_signed(WSS_CERT, WSS_KEY) == 0);

    xylem_ws_opts_t srv_opts = { .tls = &_srv_tls };
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                             _srv_echo_handler, NULL, &srv_opts);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);
    ASSERT(port != 0);

    char url[64];
    snprintf(url, sizeof(url), "wss://127.0.0.1:%u/", port);

    xylem_ws_opts_t cli_opts = { .tls = &_cli_tls };
    xylem_ws_conn_t* c = xylem_ws_dial(url, &cli_opts);
    ASSERT(c != NULL);

    const char* text = "hello secure websocket";
    ASSERT(xylem_ws_send(c, XYLEM_WS_TEXT, text, strlen(text)) == 0);

    xylem_ws_msg_t msg;
    ASSERT(xylem_ws_recv(c, &msg) == 0);
    ASSERT(msg.opcode == XYLEM_WS_TEXT);
    ASSERT(msg.len == strlen(text));
    ASSERT(memcmp(msg.data, text, msg.len) == 0);
    xylem_ws_msg_free(&msg);

    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    remove(WSS_CERT);
    remove(WSS_KEY);
    xylem_shutdown();
}

/* Test: wss binary echo. */

static void test_wss_binary_echo(void* arg) {
    (void)arg;
    ASSERT(_gen_self_signed(WSS_CERT, WSS_KEY) == 0);

    xylem_ws_opts_t srv_opts = { .tls = &_srv_tls };
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                             _srv_echo_handler, NULL, &srv_opts);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);

    char url[64];
    snprintf(url, sizeof(url), "wss://127.0.0.1:%u/", port);
    xylem_ws_opts_t cli_opts = { .tls = &_cli_tls };
    xylem_ws_conn_t* c = xylem_ws_dial(url, &cli_opts);
    ASSERT(c != NULL);

    uint8_t data[] = {0x00, 0x01, 0x02, 0xFF, 0xFE, 0x7F, 0x80};
    ASSERT(xylem_ws_send(c, XYLEM_WS_BINARY, data, sizeof(data)) == 0);

    xylem_ws_msg_t msg;
    ASSERT(xylem_ws_recv(c, &msg) == 0);
    ASSERT(msg.opcode == XYLEM_WS_BINARY);
    ASSERT(msg.len == sizeof(data));
    ASSERT(memcmp(msg.data, data, sizeof(data)) == 0);
    xylem_ws_msg_free(&msg);

    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    remove(WSS_CERT);
    remove(WSS_KEY);
    xylem_shutdown();
}

/* Test: wss multiple messages. */

static void test_wss_multiple_messages(void* arg) {
    (void)arg;
    ASSERT(_gen_self_signed(WSS_CERT, WSS_KEY) == 0);

    xylem_ws_opts_t srv_opts = { .tls = &_srv_tls };
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                             _srv_echo_handler, NULL, &srv_opts);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);

    char url[64];
    snprintf(url, sizeof(url), "wss://127.0.0.1:%u/", port);
    xylem_ws_opts_t cli_opts = { .tls = &_cli_tls };
    xylem_ws_conn_t* c = xylem_ws_dial(url, &cli_opts);
    ASSERT(c != NULL);

    for (int i = 0; i < 10; i++) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "secure-msg-%d", i);
        ASSERT(xylem_ws_send(c, XYLEM_WS_TEXT, buf, (size_t)len) == 0);

        xylem_ws_msg_t msg;
        ASSERT(xylem_ws_recv(c, &msg) == 0);
        ASSERT(msg.len == (size_t)len);
        ASSERT(memcmp(msg.data, buf, msg.len) == 0);
        xylem_ws_msg_free(&msg);
    }

    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    remove(WSS_CERT);
    remove(WSS_KEY);
    xylem_shutdown();
}

/* Test: wss large fragmented message. */

static void test_wss_large_message(void* arg) {
    (void)arg;
    ASSERT(_gen_self_signed(WSS_CERT, WSS_KEY) == 0);

    xylem_ws_opts_t srv_opts = { .fragment_threshold = 1024, .tls = &_srv_tls };
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                             _srv_echo_handler, NULL, &srv_opts);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);

    char url[64];
    snprintf(url, sizeof(url), "wss://127.0.0.1:%u/", port);
    xylem_ws_opts_t cli_opts = { .fragment_threshold = 1024, .tls = &_cli_tls };
    xylem_ws_conn_t* c = xylem_ws_dial(url, &cli_opts);
    ASSERT(c != NULL);

    size_t big_len = 8192;
    uint8_t* big = (uint8_t*)malloc(big_len);
    ASSERT(big != NULL);
    for (size_t i = 0; i < big_len; i++) {
        big[i] = (uint8_t)(i & 0xFF);
    }

    ASSERT(xylem_ws_send(c, XYLEM_WS_BINARY, big, big_len) == 0);

    xylem_ws_msg_t msg;
    ASSERT(xylem_ws_recv(c, &msg) == 0);
    ASSERT(msg.opcode == XYLEM_WS_BINARY);
    ASSERT(msg.len == big_len);
    ASSERT(memcmp(msg.data, big, big_len) == 0);
    xylem_ws_msg_free(&msg);

    free(big);
    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    remove(WSS_CERT);
    remove(WSS_KEY);
    xylem_shutdown();
}

/* Test: wss with permessage-deflate. */

static void test_wss_deflate(void* arg) {
    (void)arg;
    ASSERT(_gen_self_signed(WSS_CERT, WSS_KEY) == 0);

    xylem_ws_opts_t srv_opts = {
        .permessage_deflate = true,
        .tls                = &_srv_tls,
    };
    xylem_ws_listener_t* l = xylem_ws_listen("127.0.0.1", 0,
                                             _srv_echo_handler, NULL, &srv_opts);
    ASSERT(l != NULL);
    uint16_t port = xylem_ws_listener_port(l);

    char url[64];
    snprintf(url, sizeof(url), "wss://127.0.0.1:%u/", port);
    xylem_ws_opts_t cli_opts = {
        .permessage_deflate = true,
        .tls                = &_cli_tls,
    };
    xylem_ws_conn_t* c = xylem_ws_dial(url, &cli_opts);
    ASSERT(c != NULL);

    const char* text = "compressed payload over a TLS websocket connection!";
    ASSERT(xylem_ws_send(c, XYLEM_WS_TEXT, text, strlen(text)) == 0);

    xylem_ws_msg_t msg;
    ASSERT(xylem_ws_recv(c, &msg) == 0);
    ASSERT(msg.opcode == XYLEM_WS_TEXT);
    ASSERT(msg.len == strlen(text));
    ASSERT(memcmp(msg.data, text, msg.len) == 0);
    xylem_ws_msg_free(&msg);

    xylem_ws_close(c, 1000, NULL, 0);
    xylem_ws_close_listener(l);
    remove(WSS_CERT);
    remove(WSS_KEY);
    xylem_shutdown();
}

/* Runner. */

typedef void (*test_fn_t)(void*);

static test_fn_t tests[] = {
    test_wss_text_echo,
    test_wss_binary_echo,
    test_wss_multiple_messages,
    test_wss_large_message,
    test_wss_deflate,
};

static int test_count = (int)(sizeof(tests) / sizeof(tests[0]));

int main(void) {
    for (int i = 0; i < test_count; i++) {
        xylem_run(tests[i], NULL, NULL);
    }
    printf("All %d WSS tests passed.\n", test_count);
    return 0;
}
