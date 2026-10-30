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

#include "xylem/net/xylem-ws.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_DEFAULT_MAX_MSG_SIZE       (16 * 1024 * 1024)
#define WS_DEFAULT_FRAGMENT_THRESHOLD (16 * 1024)
#define WS_DEFAULT_HANDSHAKE_TIMEOUT  10000
#define WS_DEFAULT_CLOSE_TIMEOUT      5000
#define WS_RECV_BUF_INIT              4096

typedef struct ws_transport_s {
    void* conn;
    int (*read)(void*, void*, int);
    int (*write)(void*, const void*, int);
    void (*close)(void*);
    void (*destroy)(void*);
    void (*set_rd_deadline)(void*, uint64_t);
    void (*set_wr_deadline)(void*, uint64_t);
    int (*remote_addr)(void*, char*, size_t, uint16_t*);
    int (*local_addr)(void*, char*, size_t, uint16_t*);
} ws_transport_t;

typedef ws_transport_t (*ws_accept_fn_t)(void*);
typedef int (*ws_listener_addr_fn_t)(void*, char*, size_t, uint16_t*);

/**
 * @brief Complete a client Upgrade handshake over an owned transport.
 *
 * @param transport  Owned connection transport.
 * @param host       Host name or numeric address.
 * @param port       TCP port.
 * @param path       Request path and optional query.
 * @param opts       Options, or NULL for defaults.
 *
 * @return Owned handle on success, NULL on failure.
 *
 * @note The returned connection owns its transport and configuration resources.
 */
extern xylem_ws_conn_t* ws_dial_impl(
    ws_transport_t         transport,
    const char*            host,
    uint16_t               port,
    const char*            path,
    const xylem_ws_opts_t* opts);

/**
 * @brief Wrap an owned transport listener without starting a coroutine.
 *
 * @param handle   Owned listener, released on failure.
 * @param accept   Transport accept function.
 * @param close    Cancellation function preserving transport storage.
 * @param destroy  Final release function.
 * @param addr     Bound address query.
 * @param opts     Options, or NULL.
 *
 * @return Owned listener, or NULL on failure.
 */
extern xylem_ws_listener_t* ws_listener_create(
    void*          handle,
    ws_accept_fn_t accept,
    void (*close)(void*),
    void (*destroy)(void*),
    ws_listener_addr_fn_t  addr,
    const xylem_ws_opts_t* opts);

/**
 * @brief Bind a standalone TCP WebSocket listener.
 *
 * @param host  Bind address.
 * @param port  Bind port.
 * @param opts  Options, or NULL.
 *
 * @return Owned listener, or NULL on failure.
 */
extern xylem_ws_listener_t*
ws_tcp_listen(const char* host, uint16_t port, const xylem_ws_opts_t* opts);

/**
 * @brief Bind a standalone TLS WebSocket listener.
 *
 * @param host  Bind address.
 * @param port  Bind port.
 * @param opts  Options including TLS certificate and key.
 *
 * @return Owned listener, or NULL on failure.
 */
extern xylem_ws_listener_t*
ws_tls_listen(const char* host, uint16_t port, const xylem_ws_opts_t* opts);

/**
 * @brief Complete a serial server handshake over a borrowed transport.
 *
 * @param transport  Transport, retained by the caller on failure.
 * @param opts       Options, or NULL.
 *
 * @return Owned connection on success, NULL on failure.
 */
extern xylem_ws_conn_t*
ws_accept_impl(ws_transport_t transport, const xylem_ws_opts_t* opts);

/**
 * @brief Set a connection deadline.
 *
 * @param conn         Connection handle.
 * @param deadline_ms  Absolute deadline, or 0 to clear.
 * @param write        Select write rather than read.
 */
extern void
ws_set_deadline(xylem_ws_conn_t* conn, uint64_t deadline_ms, bool write);

/**
 * @brief Query a connection address.
 *
 * @param conn      Connection handle.
 * @param host      Address buffer, or NULL.
 * @param host_len  Buffer capacity.
 * @param port      Port output, or NULL.
 * @param remote    Select remote rather than local address.
 *
 * @return 0 on success, -1 on failure.
 */
extern int ws_addr(
    xylem_ws_conn_t* conn,
    char*            host,
    size_t           host_len,
    uint16_t*        port,
    bool             remote);

/**
 * @brief Finally release a connection after all operations return.
 *
 * @param conn  Connection handle, or NULL.
 */
extern void ws_destroy(xylem_ws_conn_t* conn);

/**
 * @brief Send one complete text or binary message.
 *
 * @note [COROUTINE-ONLY] One sending coroutine per connection, shared
 * by send and ping. The type is mapped to its protocol opcode internally.
 *
 * @param conn  Connection handle.
 * @param data  Payload bytes, or NULL for an empty message.
 * @param len   Payload length in bytes (>= 0).
 * @param type  Message type.
 *
 * @return 0 on success, -1 on invalid arguments, I/O error or timeout.
 */
extern int ws_send(
    xylem_ws_conn_t*  conn,
    const void*       data,
    int               len,
    xylem_ws_opcode_t type);

/**
 * @brief Receive one complete message into a caller-owned buffer.
 *
 * @note [COROUTINE-ONLY] One receiving coroutine per connection; one
 * sender may run concurrently. Never truncates or combines messages.
 * A short buffer returns -1 without modifying buf or type; the message
 * remains pending for a retry with a larger buffer. I/O/protocol errors
 * and timeouts permanently close the connection. Text is not terminated.
 *
 * @param conn  Connection handle.
 * @param buf   Output buffer, or NULL when len is 0.
 * @param len   Buffer capacity in bytes (>= 0).
 * @param type  Receives the message type on success, or NULL.
 *
 * @return Message length (>= 0), including 0 for an empty message;
 *         -1 on invalid arguments, short buffer, close, error or timeout.
 */
extern int
ws_recv(xylem_ws_conn_t* conn, void* buf, int len, xylem_ws_opcode_t* type);

/**
 * @brief Send a ping frame.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param conn  Connection handle.
 * @param data  Optional ping payload.
 * @param len   Payload length.
 *
 * @return 0 on success, -1 on error.
 */
extern int ws_ping(xylem_ws_conn_t* conn, const void* data, size_t len);

/**
 * @brief Initiate a close handshake.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param conn        Connection handle.
 * @param code        Close status code (RFC 6455).
 * @param reason      Optional close reason string.
 * @param reason_len  Length of reason.
 *
 * @return 0 on success, -1 on error.
 */
extern int ws_close(
    xylem_ws_conn_t* conn,
    uint16_t         code,
    const char*      reason,
    size_t           reason_len);

/**
 * @brief Get the user data pointer attached to the connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param conn  Connection handle.
 *
 * @return User data pointer.
 */
extern void* ws_get_userdata(xylem_ws_conn_t* conn);

/**
 * @brief Set the user data pointer attached to the connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param conn  Connection handle.
 * @param ud    User data pointer.
 */
extern void ws_set_userdata(xylem_ws_conn_t* conn, void* ud);
