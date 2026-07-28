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
 * Stub TLS API for builds without TLS support. Every public symbol remains
 * linkable and reports that TLS is unavailable.
 */

#include "xylem/net/xylem-tls.h"

xylem_tls_ctx_t* xylem_tls_ctx_create(void) {
    return NULL;
}

void xylem_tls_ctx_destroy(xylem_tls_ctx_t* ctx) {
    (void)ctx;
}

int xylem_tls_ctx_load_cert(
    xylem_tls_ctx_t* ctx,
    const char*      hostname,
    const char*      cert,
    const char*      key) {
    (void)ctx;
    (void)hostname;
    (void)cert;
    (void)key;
    return -1;
}

int xylem_tls_ctx_load_cert_mem(
    xylem_tls_ctx_t* ctx,
    const char*      hostname,
    const void*      cert_pem,
    size_t           cert_len,
    const void*      key_pem,
    size_t           key_len) {
    (void)ctx;
    (void)hostname;
    (void)cert_pem;
    (void)cert_len;
    (void)key_pem;
    (void)key_len;
    return -1;
}

int xylem_tls_ctx_load_ca(xylem_tls_ctx_t* ctx, const char* ca_file) {
    (void)ctx;
    (void)ca_file;
    return -1;
}

int xylem_tls_ctx_load_system_ca(
    xylem_tls_ctx_t* ctx,
    const char*      fallback_ca_file) {
    (void)ctx;
    (void)fallback_ca_file;
    return -1;
}

void xylem_tls_ctx_verify_server(xylem_tls_ctx_t* ctx, bool enable) {
    (void)ctx;
    (void)enable;
}

void xylem_tls_ctx_verify_client(xylem_tls_ctx_t* ctx, bool enable) {
    (void)ctx;
    (void)enable;
}

int xylem_tls_ctx_set_alpn(
    xylem_tls_ctx_t* ctx,
    const char**     protocols,
    size_t           count) {
    (void)ctx;
    (void)protocols;
    (void)count;
    return -1;
}

int xylem_tls_ctx_set_keylog(xylem_tls_ctx_t* ctx, const char* path) {
    (void)ctx;
    (void)path;
    return -1;
}

xylem_tls_conn_t* xylem_tls_dial(
    const char*       host,
    uint16_t          port,
    xylem_tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts) {
    (void)host;
    (void)port;
    (void)ctx;
    (void)opts;
    return NULL;
}

xylem_tls_listener_t* xylem_tls_listen(
    const char*       host,
    uint16_t          port,
    xylem_tls_ctx_t*  ctx,
    xylem_tls_opts_t* opts) {
    (void)host;
    (void)port;
    (void)ctx;
    (void)opts;
    return NULL;
}

xylem_tls_conn_t* xylem_tls_accept(xylem_tls_listener_t* ln) {
    (void)ln;
    return NULL;
}

void xylem_tls_set_read_deadline(
    xylem_tls_conn_t* tls,
    uint64_t          deadline_ms) {
    (void)tls;
    (void)deadline_ms;
}

void xylem_tls_set_write_deadline(
    xylem_tls_conn_t* tls,
    uint64_t          deadline_ms) {
    (void)tls;
    (void)deadline_ms;
}

void xylem_tls_close(xylem_tls_conn_t* tls) {
    (void)tls;
}

int xylem_tls_read(xylem_tls_conn_t* tls, void* buf, int len) {
    (void)tls;
    (void)buf;
    (void)len;
    return -1;
}

int xylem_tls_write(xylem_tls_conn_t* tls, const void* data, int len) {
    (void)tls;
    (void)data;
    (void)len;
    return -1;
}

void xylem_tls_destroy(xylem_tls_conn_t* tls) {
    (void)tls;
}

void xylem_tls_close_listener(xylem_tls_listener_t* ln) {
    (void)ln;
}

void xylem_tls_destroy_listener(xylem_tls_listener_t* ln) {
    (void)ln;
}

int xylem_tls_remote_addr(
    xylem_tls_conn_t* tls,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    (void)tls;
    (void)host;
    (void)host_len;
    (void)port;
    return -1;
}

int xylem_tls_local_addr(
    xylem_tls_conn_t* tls,
    char*             host,
    size_t            host_len,
    uint16_t*         port) {
    (void)tls;
    (void)host;
    (void)host_len;
    (void)port;
    return -1;
}

int xylem_tls_listener_addr(
    xylem_tls_listener_t* ln,
    char*                 host,
    size_t                host_len,
    uint16_t*             port) {
    (void)ln;
    (void)host;
    (void)host_len;
    (void)port;
    return -1;
}

const char* xylem_tls_get_alpn(xylem_tls_conn_t* tls) {
    (void)tls;
    return NULL;
}

int xylem_tls_handshake(xylem_tls_conn_t* tls) {
    (void)tls;
    return -1;
}
