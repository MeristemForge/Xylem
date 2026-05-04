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

#include "xylem/net/xylem-tcp.h"

typedef int xylem_tcp_timeout_type_t;

typedef struct xylem_tls_conn_s   xylem_tls_conn_t;
typedef struct xylem_tls_ctx_s    xylem_tls_ctx_t;
typedef struct xylem_tls_server_s xylem_tls_server_t;

/* TLS connection options. */
typedef struct xylem_tls_opts_s {
    xylem_tcp_opts_t tcp;           /*< Underlying TCP options. */
    uint64_t         connect_timeout_ms; /*< Connect timeout, 0 = no timeout. */
    const char*      hostname;   /*< SNI hostname for server certificate selection and hostname verification. */
} xylem_tls_opts_t;

/* TLS event callback set. */
typedef struct xylem_tls_handler_s {
    void (*on_connect)(xylem_tls_conn_t* tls);
    void (*on_accept)(xylem_tls_server_t* server,
                      xylem_tls_conn_t* tls);
    void (*on_read)(xylem_tls_conn_t* tls,
                    void* data, size_t len);
    void (*on_write_done)(xylem_tls_conn_t* tls,
                          const void* data, size_t len,
                          int status);
    void (*on_timeout)(xylem_tls_conn_t* tls,
                       xylem_tcp_timeout_type_t type);
    void (*on_close)(xylem_tls_conn_t* tls,
                     int err, const char* errmsg);
    void (*on_heartbeat_miss)(xylem_tls_conn_t* tls);
} xylem_tls_handler_t;

extern xylem_tls_ctx_t* xylem_tls_ctx_create(void);
extern void xylem_tls_ctx_destroy(xylem_tls_ctx_t* ctx);
extern int xylem_tls_ctx_load_cert(xylem_tls_ctx_t* ctx,
                                   const char* cert, const char* key);
extern int xylem_tls_ctx_set_ca(xylem_tls_ctx_t* ctx, const char* ca_file);
extern void xylem_tls_ctx_set_verify(xylem_tls_ctx_t* ctx, bool enable);
extern int xylem_tls_ctx_set_alpn(xylem_tls_ctx_t* ctx,
                                  const char** protocols, size_t count);
extern int xylem_tls_ctx_set_keylog(xylem_tls_ctx_t* ctx, const char* path);

/**
 * @brief Initiate an asynchronous TLS connection.
 *
 * @param loop     Event loop.
 * @param host     Target address string.
 * @param port     Target port.
 * @param ctx      TLS context.
 * @param handler  Event callback set.
 * @param opts     TLS options, NULL for defaults.
 *
 * @return TLS connection handle, or NULL on failure.
 */
extern xylem_tls_conn_t* xylem_tls_dial(const char* host,
                                   uint16_t port,
                                   xylem_tls_ctx_t* ctx,
                                   xylem_tls_handler_t* handler,
                                   xylem_tls_opts_t* opts);

extern int xylem_tls_send(xylem_tls_conn_t* tls,
                          const void* data, size_t len);
extern void xylem_tls_close(xylem_tls_conn_t* tls);
extern void xylem_tls_conn_acquire(xylem_tls_conn_t* tls);
extern void xylem_tls_conn_release(xylem_tls_conn_t* tls);
extern const char* xylem_tls_get_alpn(xylem_tls_conn_t* tls);

/**
 * @brief Get the peer address of a TLS connection.
 *
 * @param tls   TLS connection handle.
 * @param host  Output buffer, must be at least XYLEM_ADDR_MAXHOST bytes.
 * @param port  Output port number.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_tls_remote_addr(xylem_tls_conn_t* tls,
                                 char host[XYLEM_ADDR_MAXHOST],
                                 uint16_t* port);

extern void* xylem_tls_get_userdata(xylem_tls_conn_t* tls);
extern void xylem_tls_set_userdata(xylem_tls_conn_t* tls, void* ud);

/**
 * @brief Create a TLS server and start listening.
 *
 * @param loop     Event loop.
 * @param host     Bind address string.
 * @param port     Bind port.
 * @param ctx      TLS context with cert+key loaded.
 * @param handler  Event callback set.
 * @param opts     TLS options, NULL for defaults.
 *
 * @return Server handle, or NULL on failure.
 */
extern xylem_tls_server_t* xylem_tls_listen(const char* host,
                                            uint16_t port,
                                            xylem_tls_ctx_t* ctx,
                                            xylem_tls_handler_t* handler,
                                            xylem_tls_opts_t* opts);

extern void xylem_tls_close_server(xylem_tls_server_t* server);
extern void* xylem_tls_server_get_userdata(xylem_tls_server_t* server);
extern void xylem_tls_server_set_userdata(xylem_tls_server_t* server,
                                          void* ud);
