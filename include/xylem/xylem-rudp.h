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

#include "xylem/xylem-addr.h"
#include "xylem/xylem-loop.h"

#include <stdint.h>

typedef struct xylem_rudp_s        xylem_rudp_t;
typedef struct xylem_rudp_ctx_s    xylem_rudp_ctx_t;
typedef struct xylem_rudp_server_s xylem_rudp_server_t;

typedef struct xylem_rudp_handler_s {
    void (*on_connect)(xylem_rudp_t* rudp);
    void (*on_accept)(xylem_rudp_server_t* server, xylem_rudp_t* rudp);
    void (*on_read)(xylem_rudp_t* rudp, void* data, size_t len);
    void (*on_close)(xylem_rudp_t* rudp, int err, const char* errmsg);
} xylem_rudp_handler_t;

typedef enum xylem_rudp_mode_e {
    XYLEM_RUDP_MODE_DEFAULT, /**< Normal ARQ, 100ms interval. */
    XYLEM_RUDP_MODE_FAST,    /**< Nodelay + fast resend + no congestion. */
} xylem_rudp_mode_t;

typedef struct xylem_rudp_opts_s {
    xylem_rudp_mode_t mode; /**< Transport mode preset. */
    int      snd_wnd;       /**< Send window size, 0 for default (32). */
    int      rcv_wnd;       /**< Receive window size, 0 for default (128). */
    int      mtu;           /**< MTU size, 0 for default (1400). */
    bool     stream;        /**< true: byte-stream mode, false: message mode. */
    uint64_t timeout_ms;    /**< Dead-link timeout in ms, 0 to disable. */
    uint64_t handshake_ms;  /**< Handshake timeout in ms, 0 for default (5000). */
} xylem_rudp_opts_t;

/**
 * @brief Create a RUDP context.
 *
 * Allocates a reusable context that manages KCP conversation ID
 * generation. The initial conv is seeded from a PRNG so IDs are
 * unique across restarts. A single context can be shared by
 * multiple connections and servers.
 *
 * @return Context handle, or NULL on failure.
 */
extern xylem_rudp_ctx_t* xylem_rudp_ctx_create(void);

/**
 * @brief Destroy a RUDP context.
 *
 * @param ctx  Context handle.
 */
extern void xylem_rudp_ctx_destroy(xylem_rudp_ctx_t* ctx);

/**
 * @brief Initiate a reliable UDP connection.
 *
 * Creates a connected UDP socket to the target address and starts
 * a KCP session with an auto-assigned conversation ID. A lightweight
 * handshake confirms the peer before handler->on_connect fires.
 *
 * @param loop     Event loop.
 * @param addr     Target address.
 * @param ctx      RUDP context for conv ID generation.
 * @param handler  Event callback set.
 * @param opts     RUDP options, NULL for defaults.
 *
 * @return RUDP handle, or NULL on failure.
 */
extern xylem_rudp_t* xylem_rudp_dial(xylem_loop_t* loop,
                                     xylem_addr_t* addr,
                                     xylem_rudp_ctx_t* ctx,
                                     xylem_rudp_handler_t* handler,
                                     xylem_rudp_opts_t* opts);

/**
 * @brief Send data over a reliable UDP connection.
 *
 * Data is enqueued into the KCP send buffer and transmitted
 * on the next update cycle.
 *
 * @param rudp  RUDP handle.
 * @param data  Data to send.
 * @param len   Data length in bytes.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_rudp_send(xylem_rudp_t* rudp,
                           const void* data, size_t len);

/**
 * @brief Close a reliable UDP connection.
 *
 * Releases the KCP session and closes the underlying UDP socket.
 * handler->on_close fires with err=0 for a normal close.
 *
 * @param rudp  RUDP handle.
 */
extern void xylem_rudp_close(xylem_rudp_t* rudp);

/**
 * @brief Get the peer address of a connection.
 *
 * @param rudp  RUDP handle.
 *
 * @return Peer address.
 */
extern const xylem_addr_t* xylem_rudp_get_peer_addr(xylem_rudp_t* rudp);

/**
 * @brief Get the event loop associated with a connection.
 *
 * @param rudp  RUDP handle.
 *
 * @return Loop handle.
 */
extern xylem_loop_t* xylem_rudp_get_loop(xylem_rudp_t* rudp);

/**
 * @brief Get user data attached to a connection.
 *
 * @param rudp  RUDP handle.
 *
 * @return User data pointer.
 */
extern void* xylem_rudp_get_userdata(xylem_rudp_t* rudp);

/**
 * @brief Set user data on a connection.
 *
 * @param rudp  RUDP handle.
 * @param ud    User data pointer.
 */
extern void xylem_rudp_set_userdata(xylem_rudp_t* rudp, void* ud);

/**
 * @brief Create a reliable UDP server and start listening.
 *
 * Binds a UDP socket and demuxes incoming KCP sessions by
 * (peer address, conv). handler->on_accept fires per new session.
 *
 * @param loop     Event loop.
 * @param addr     Bind address.
 * @param ctx      RUDP context.
 * @param handler  Event callback set.
 * @param opts     RUDP options, NULL for defaults.
 *
 * @return Server handle, or NULL on failure.
 */
extern xylem_rudp_server_t* xylem_rudp_listen(xylem_loop_t* loop,
                                              xylem_addr_t* addr,
                                              xylem_rudp_ctx_t* ctx,
                                              xylem_rudp_handler_t* handler,
                                              xylem_rudp_opts_t* opts);

/**
 * @brief Close a reliable UDP server.
 *
 * Closes all active sessions and the underlying UDP socket.
 *
 * @param server  Server handle.
 */
extern void xylem_rudp_close_server(xylem_rudp_server_t* server);

/**
 * @brief Get user data attached to a server.
 *
 * @param server  Server handle.
 *
 * @return User data pointer.
 */
extern void* xylem_rudp_server_get_userdata(xylem_rudp_server_t* server);

/**
 * @brief Set user data on a server.
 *
 * @param server  Server handle.
 * @param ud      User data pointer.
 */
extern void xylem_rudp_server_set_userdata(xylem_rudp_server_t* server,
                                           void* ud);
