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
    XYLEM_WS_TEXT   = 0x1,
    XYLEM_WS_BINARY = 0x2,
} xylem_ws_opcode_t;

typedef struct {
    xylem_ws_opcode_t opcode;
    void*             data;
    size_t            len;
} xylem_ws_msg_t;

typedef void (*xylem_ws_handler_fn_t)(xylem_ws_conn_t* conn, void* userdata);

/**
 * TLS configuration for secure WebSocket (wss), by certificate/CA files;
 * no tls context is exposed. Mirrors the HTTP server/client TLS config:
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

typedef struct {
    size_t   max_msg_size;
    size_t   fragment_threshold;
    uint64_t handshake_timeout_ms;
    uint64_t close_timeout_ms;
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

struct xylem_http_res_s;
struct xylem_http_req_s;

/**
 * @brief Upgrade an HTTP request to a WebSocket connection (server side).
 *
 * @param res   HTTP response handle.
 * @param req   HTTP request handle.
 * @param opts  WebSocket options, or NULL for defaults.
 *
 * @return WebSocket connection, or NULL on failure.
 */
extern xylem_ws_conn_t* xylem_ws_accept(struct xylem_http_res_s* res,
                                         struct xylem_http_req_s* req,
                                         const xylem_ws_opts_t* opts);

/**
 * @brief Start a standalone WebSocket server (ws or wss).
 *
 * Plain ws by default; set opts->tls (cert + key) to serve wss over TLS.
 *
 * @param host      Bind address, or NULL for any.
 * @param port      Bind port.
 * @param handler   Connection handler invoked per accepted client.
 * @param userdata  Opaque pointer passed to handler.
 * @param opts      WebSocket options (opts->tls selects wss), or NULL.
 *
 * @return Listener handle, or NULL on failure.
 */
extern xylem_ws_listener_t* xylem_ws_listen(const char* host, uint16_t port,
                                             xylem_ws_handler_fn_t handler,
                                             void* userdata,
                                             const xylem_ws_opts_t* opts);

/**
 * @brief Close a WebSocket listener and stop accepting connections.
 *
 * @param listener  Listener handle.
 */
extern void     xylem_ws_close_listener(xylem_ws_listener_t* listener);

/**
 * @brief Return the port the listener is bound to.
 *
 * @param listener  Listener handle.
 *
 * @return Bound port number.
 */
extern uint16_t xylem_ws_listener_port(xylem_ws_listener_t* listener);

/**
 * @brief Connect to a WebSocket server (ws or wss).
 *
 * The URL scheme selects transport: ws:// dials plain TCP, wss:// dials
 * over TLS using opts->tls (NULL = default system trust).
 *
 * @param url   WebSocket URL (ws://host:port/path or wss://...).
 * @param opts  WebSocket options (opts->tls used for wss), or NULL.
 *
 * @return WebSocket connection, or NULL on failure.
 */
extern xylem_ws_conn_t* xylem_ws_dial(const char* url,
                                       const xylem_ws_opts_t* opts);

/**
 * @brief Send a message on the connection.
 *
 * @param conn    Connection handle.
 * @param opcode  Message type (text or binary).
 * @param data    Payload bytes.
 * @param len     Payload length.
 *
 * @return 0 on success, -1 on error.
 */
extern int  xylem_ws_send(xylem_ws_conn_t* conn, xylem_ws_opcode_t opcode,
                          const void* data, size_t len);

/**
 * @brief Receive the next message (blocks until available).
 *
 * @param conn  Connection handle.
 * @param msg   Output message structure.
 *
 * @return 0 on success, -1 on error or connection closed.
 */
extern int  xylem_ws_recv(xylem_ws_conn_t* conn, xylem_ws_msg_t* msg);

/**
 * @brief Send a ping frame.
 *
 * @param conn  Connection handle.
 * @param data  Optional ping payload.
 * @param len   Payload length.
 *
 * @return 0 on success, -1 on error.
 */
extern int  xylem_ws_ping(xylem_ws_conn_t* conn, const void* data, size_t len);

/**
 * @brief Initiate a close handshake.
 *
 * @param conn        Connection handle.
 * @param code        Close status code (RFC 6455).
 * @param reason      Optional close reason string.
 * @param reason_len  Length of reason.
 *
 * @return 0 on success, -1 on error.
 */
extern int  xylem_ws_close(xylem_ws_conn_t* conn, uint16_t code,
                           const char* reason, size_t reason_len);

/**
 * @brief Free resources owned by a received message.
 *
 * @param msg  Message to free.
 */
extern void xylem_ws_msg_free(xylem_ws_msg_t* msg);

/**
 * @brief Return the close code received from the peer.
 *
 * @param conn  Connection handle.
 *
 * @return Close code, or 0 if not yet received.
 */
extern uint16_t xylem_ws_close_code(xylem_ws_conn_t* conn);

/**
 * @brief Get the user data pointer attached to the connection.
 *
 * @param conn  Connection handle.
 *
 * @return User data pointer.
 */
extern void*    xylem_ws_get_userdata(xylem_ws_conn_t* conn);

/**
 * @brief Set the user data pointer attached to the connection.
 *
 * @param conn  Connection handle.
 * @param ud    User data pointer.
 */
extern void     xylem_ws_set_userdata(xylem_ws_conn_t* conn, void* ud);
