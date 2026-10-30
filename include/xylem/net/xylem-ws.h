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

typedef struct xylem_ws_conn_s     xylem_ws_conn_t;
typedef struct xylem_ws_listener_s xylem_ws_listener_t;

typedef enum xylem_ws_opcode_e {
    XYLEM_WS_TEXT,
    XYLEM_WS_BINARY,
} xylem_ws_opcode_t;

/**
 * TLS configuration for secure WebSocket (wss), by certificate/CA files;
 * no TLS context is exposed:
 *   - server (xylem_ws_listen with a wss listener): cert + key are
 *     required; ca enables client-certificate verification (mTLS).
 *   - client (xylem_ws_dial of a wss:// URL): ca pins a private CA
 *     (NULL = system trust); skip_verify disables server verification
 *     (tests only).
 */
typedef struct xylem_ws_tls_s {
    const char* cert;        /* PEM certificate path (server). */
    const char* key;         /* PEM private key path (server). */
    const char* ca;          /* CA path, NULL = system default. */
    bool        skip_verify; /* true = skip peer cert verification (client). */
} xylem_ws_tls_t;

typedef struct xylem_ws_opts_s {
    size_t   max_msg_size;
    size_t   fragment_threshold;
    uint64_t handshake_timeout_ms; /* Total handshake budget, 0 = 10 seconds. */
    uint64_t close_timeout_ms;     /* Close handshake budget, 0 = 5 seconds. */
    bool     permessage_deflate;
    bool     deflate_context_takeover;
    /**
     * TLS configuration. Server: non-NULL makes xylem_ws_listen() a wss
     * (TLS) listener; NULL makes it plain ws. Client: used only when the
     * dialed URL is wss://; NULL means default TLS (system trust). When a
     * wss:// URL is dialed with NULL opts, default TLS is used.
     */
    const xylem_ws_tls_t* tls;
} xylem_ws_opts_t;

/**
 * @brief Bind a standalone ws or wss listener.
 *
 * @note [COROUTINE-ONLY] Set opts->tls (cert + key) to serve wss.
 *
 * @param host  Bind address, or NULL for any.
 * @param port  Bind port, or 0 for an assigned port.
 * @param opts  Options, or NULL for defaults.
 *
 * @return Owned listener, or NULL on failure.
 */
extern xylem_ws_listener_t*
xylem_ws_listen(const char* host, uint16_t port, const xylem_ws_opts_t* opts);

/**
 * @brief Accept a connection and complete its TLS and WS handshakes.
 *
 * @note [COROUTINE-ONLY] One accepting coroutine per listener. Handshakes
 * run serially with handshake_timeout_ms (0 selects 10 seconds). Invalid
 * or timed-out peers are discarded before accepting the next peer.
 * The caller owns the returned connection and chooses whether to spawn
 * a coroutine. Closing the listener also cancels an in-flight handshake.
 *
 * @param listener  Listener handle.
 *
 * @return Owned connection, or NULL on listener close or accept failure.
 */
extern xylem_ws_conn_t* xylem_ws_accept(xylem_ws_listener_t* listener);

/**
 * @brief Stop accepting connections and cancel a pending handshake.
 *
 * @note [COROUTINE-ONLY] Idempotent; may race with accept. Existing
 * returned connections remain usable. The listener handle stays alive.
 *
 * @param listener  Listener handle, or NULL.
 */
extern void xylem_ws_close_listener(xylem_ws_listener_t* listener);

/**
 * @brief Release a listener after all its operations have returned.
 *
 * @note [COROUTINE-ONLY] Final, non-concurrent operation on the handle.
 *
 * @param listener  Listener handle, or NULL.
 */
extern void xylem_ws_destroy_listener(xylem_ws_listener_t* listener);

/**
 * @brief Get the bound listener address, including an assigned port.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param listener  Listener handle.
 * @param host      Address output buffer, or NULL.
 * @param host_len  Output buffer capacity.
 * @param port      Port output, or NULL.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_ws_listener_addr(
    xylem_ws_listener_t* listener,
    char*                host,
    size_t               host_len,
    uint16_t*            port);

/**
 * @brief Connect to a WebSocket server (ws or wss).
 *
 * @note [COROUTINE-ONLY]
 *
 * The URL scheme selects transport: ws:// dials plain TCP, wss:// dials
 * over TLS using opts->tls (NULL = default system trust).
 *
 * @param url   WebSocket URL (ws://host:port/path or wss://...).
 * @param opts  WebSocket options (opts->tls used for wss), or NULL.
 *
 * @return WebSocket connection, or NULL on failure.
 */
extern xylem_ws_conn_t*
xylem_ws_dial(const char* url, const xylem_ws_opts_t* opts);

/**
 * @brief Set an absolute read deadline for complete messages.
 *
 * @note [COROUTINE-ONLY] Uses xylem_utils_getnow(MSEC) timestamps, as TCP
 * does. Applies to in-flight and subsequent recv calls, including buffered
 * messages and all fragments. I/O/protocol failures permanently close the
 * connection; clearing the deadline does not recover it. A buffer-size
 * error retains the message and does not close the connection.
 *
 * @param conn         Connection handle.
 * @param deadline_ms  Absolute deadline, or 0 to clear.
 */
extern void
xylem_ws_set_read_deadline(xylem_ws_conn_t* conn, uint64_t deadline_ms);

/**
 * @brief Set an absolute write deadline for messages and control frames.
 *
 * @note [COROUTINE-ONLY] Same clock and clearing rule as the read deadline.
 * Covers the whole message, not each fragment separately. A failed write
 * permanently closes the connection.
 *
 * @param conn         Connection handle.
 * @param deadline_ms  Absolute deadline, or 0 to clear.
 */
extern void
xylem_ws_set_write_deadline(xylem_ws_conn_t* conn, uint64_t deadline_ms);

/**
 * @brief Get the remote connection address.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param conn      Connection handle.
 * @param host      Address output buffer, or NULL.
 * @param host_len  Output buffer capacity.
 * @param port      Port output, or NULL.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_ws_remote_addr(
    xylem_ws_conn_t* conn,
    char*            host,
    size_t           host_len,
    uint16_t*        port);

/**
 * @brief Get the local connection address.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param conn      Connection handle.
 * @param host      Address output buffer, or NULL.
 * @param host_len  Output buffer capacity.
 * @param port      Port output, or NULL.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_ws_local_addr(
    xylem_ws_conn_t* conn,
    char*            host,
    size_t           host_len,
    uint16_t*        port);

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
extern int xylem_ws_send(
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
extern int xylem_ws_recv(
    xylem_ws_conn_t*   conn,
    void*              buf,
    int                len,
    xylem_ws_opcode_t* type);

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
extern int xylem_ws_ping(xylem_ws_conn_t* conn, const void* data, size_t len);

/**
 * @brief Close a connection without releasing its handle.
 *
 * @note [COROUTINE-ONLY]
 *
 * When no send/recv/ping is active, attempts a close handshake bounded by
 * close_timeout_ms and any earlier read/write deadlines. When I/O is active,
 * cancels it immediately instead. Idempotent and safe to race with I/O.
 * Wait for all operations to return before calling xylem_ws_destroy().
 *
 * @param conn        Connection handle.
 * @param code        Close status code (RFC 6455).
 * @param reason      Optional close reason string.
 * @param reason_len  Length of reason.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_ws_close(
    xylem_ws_conn_t* conn,
    uint16_t         code,
    const char*      reason,
    size_t           reason_len);

/**
 * @brief Release a connection after all its operations have returned.
 *
 * @note [COROUTINE-ONLY] Final, non-concurrent call. Cancels communication
 * if needed; call close first when a graceful handshake is desired.
 *
 * @param conn  Connection handle, or NULL.
 */
extern void xylem_ws_destroy(xylem_ws_conn_t* conn);

/**
 * @brief Get the user data pointer attached to the connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param conn  Connection handle.
 *
 * @return User data pointer.
 */
extern void* xylem_ws_get_userdata(xylem_ws_conn_t* conn);

/**
 * @brief Set the user data pointer attached to the connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param conn  Connection handle.
 * @param ud    User data pointer.
 */
extern void xylem_ws_set_userdata(xylem_ws_conn_t* conn, void* ud);
