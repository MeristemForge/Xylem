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

_Pragma("once")

#include "xylem/xylem-error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct xylem_dtls_conn_s     xylem_dtls_conn_t;
typedef struct xylem_dtls_ctx_s      xylem_dtls_ctx_t;
typedef struct xylem_dtls_listener_s xylem_dtls_listener_t;

typedef struct xylem_dtls_opts_s {
    uint64_t    connect_timeout_ms;
    const char* hostname;
} xylem_dtls_opts_t;

extern xylem_dtls_ctx_t* xylem_dtls_ctx_create(void);
extern void xylem_dtls_ctx_destroy(xylem_dtls_ctx_t* ctx);
extern int xylem_dtls_ctx_load_cert(xylem_dtls_ctx_t* ctx,
                                    const char* cert, const char* key);
extern int xylem_dtls_ctx_set_ca(xylem_dtls_ctx_t* ctx,
                                 const char* ca_file);
extern void xylem_dtls_ctx_set_verify(xylem_dtls_ctx_t* ctx, bool enable);
extern int xylem_dtls_ctx_set_alpn(xylem_dtls_ctx_t* ctx,
                                   const char** protocols, size_t count);
extern int xylem_dtls_ctx_set_keylog(xylem_dtls_ctx_t* ctx,
                                     const char* path);

extern xylem_dtls_conn_t* xylem_dtls_dial(
    const char* host, uint16_t port,
    xylem_dtls_ctx_t* ctx, xylem_dtls_opts_t* opts);

extern xylem_dtls_listener_t* xylem_dtls_listen(
    const char* host, uint16_t port,
    xylem_dtls_ctx_t* ctx, xylem_dtls_opts_t* opts);

extern xylem_dtls_conn_t* xylem_dtls_accept(xylem_dtls_listener_t* ln);

extern int64_t xylem_dtls_recv(
    xylem_dtls_conn_t* dtls, void* buf, size_t len);

extern int xylem_dtls_send(
    xylem_dtls_conn_t* dtls, const void* data, size_t len);

extern void xylem_dtls_set_read_deadline(
    xylem_dtls_conn_t* dtls, uint64_t deadline_ms);
extern void xylem_dtls_set_write_deadline(
    xylem_dtls_conn_t* dtls, uint64_t deadline_ms);

extern void xylem_dtls_close(xylem_dtls_conn_t* dtls);
extern void xylem_dtls_close_listener(xylem_dtls_listener_t* ln);

extern xylem_err_t xylem_dtls_get_error(xylem_dtls_conn_t* dtls);
extern const char* xylem_dtls_get_alpn(xylem_dtls_conn_t* dtls);

extern int xylem_dtls_remote_addr(
    xylem_dtls_conn_t* dtls,
    char* host, size_t host_len, uint16_t* port);
extern int xylem_dtls_local_addr(
    xylem_dtls_conn_t* dtls,
    char* host, size_t host_len, uint16_t* port);
extern int xylem_dtls_listener_addr(
    xylem_dtls_listener_t* ln,
    char* host, size_t host_len, uint16_t* port);
