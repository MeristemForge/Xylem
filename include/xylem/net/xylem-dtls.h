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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef XYLEM_ADDR_MAXHOST
#define XYLEM_ADDR_MAXHOST 46
#endif

typedef struct xylem_dtls_conn_s   xylem_dtls_conn_t;
typedef struct xylem_dtls_ctx_s    xylem_dtls_ctx_t;
typedef struct xylem_dtls_server_s xylem_dtls_server_t;

/* DTLS event callback set. */
typedef struct xylem_dtls_handler_s {
    void (*on_connect)(xylem_dtls_conn_t* dtls);
    void (*on_accept)(xylem_dtls_server_t* server,
                      xylem_dtls_conn_t* dtls);
    void (*on_read)(xylem_dtls_conn_t* dtls,
                    void* data, size_t len);
    void (*on_close)(xylem_dtls_conn_t* dtls,
                     int err, const char* errmsg);
} xylem_dtls_handler_t;

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

/**
 * @brief Initiate an asynchronous DTLS connection.
 *
 * @param loop     Event loop.
 * @param host     Target address string.
 * @param port     Target port.
 * @param ctx      DTLS context.
 * @param handler  Event callback set.
 *
 * @return DTLS session handle, or NULL on failure.
 */
extern xylem_dtls_conn_t* xylem_dtls_dial(const char* host,
                                     uint16_t port,
                                     xylem_dtls_ctx_t* ctx,
                                     xylem_dtls_handler_t* handler);

extern int xylem_dtls_send(xylem_dtls_conn_t* dtls,
                           const void* data, size_t len);
extern void xylem_dtls_close(xylem_dtls_conn_t* dtls);
extern void xylem_dtls_conn_ref(xylem_dtls_conn_t* dtls);
extern void xylem_dtls_conn_unref(xylem_dtls_conn_t* dtls);
extern const char* xylem_dtls_get_alpn(xylem_dtls_conn_t* dtls);

/**
 * @brief Get the peer address of a DTLS session.
 *
 * @param dtls  DTLS session handle.
 * @param host  Output buffer, must be at least XYLEM_ADDR_MAXHOST bytes.
 * @param port  Output port number.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_dtls_remote_addr(xylem_dtls_conn_t* dtls,
                                  char host[XYLEM_ADDR_MAXHOST],
                                  uint16_t* port);

extern void* xylem_dtls_get_userdata(xylem_dtls_conn_t* dtls);
extern void xylem_dtls_set_userdata(xylem_dtls_conn_t* dtls, void* ud);

/**
 * @brief Create a DTLS server and start listening.
 *
 * @param loop     Event loop.
 * @param host     Bind address string.
 * @param port     Bind port.
 * @param ctx      DTLS context with cert+key loaded.
 * @param handler  Event callback set.
 *
 * @return Server handle, or NULL on failure.
 */
extern xylem_dtls_server_t* xylem_dtls_listen(const char* host,
                                              uint16_t port,
                                              xylem_dtls_ctx_t* ctx,
                                              xylem_dtls_handler_t* handler);

extern void xylem_dtls_close_server(xylem_dtls_server_t* server);
extern void* xylem_dtls_server_get_userdata(xylem_dtls_server_t* server);
extern void xylem_dtls_server_set_userdata(xylem_dtls_server_t* server,
                                           void* ud);
