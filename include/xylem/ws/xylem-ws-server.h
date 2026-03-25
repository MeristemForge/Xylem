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

#include "xylem/ws/xylem-ws-common.h"
#include "xylem/xylem-loop.h"

#include <stddef.h>
#include <stdint.h>

/** WebSocket server configuration. */
typedef struct xylem_ws_srv_cfg_s {
    const char*         host;       /**< Bind address (e.g. "0.0.0.0"). */
    uint16_t            port;       /**< Bind port. */
    xylem_ws_handler_t* handler;    /**< Event callback set. */
    xylem_ws_opts_t*    opts;       /**< Connection options, NULL for defaults. */
    const char*         tls_cert;   /**< PEM cert path, NULL for plain ws://. */
    const char*         tls_key;    /**< PEM key path, NULL for plain ws://. */
} xylem_ws_srv_cfg_t;

/**
 * @brief Create a WebSocket server and start listening.
 *
 * Binds to the address specified in cfg. When tls_cert and tls_key
 * are provided, the server accepts wss:// connections over TLS;
 * otherwise it accepts plain ws:// connections over TCP.
 *
 * @param loop  Event loop.
 * @param cfg   Server configuration.
 *
 * @return Server handle, or NULL on failure.
 */
extern xylem_ws_server_t* xylem_ws_listen(xylem_loop_t* loop,
                                          const xylem_ws_srv_cfg_t* cfg);

/**
 * @brief Close a WebSocket server.
 *
 * Stops accepting new connections and frees the server handle.
 * Existing connections are not affected and must be closed
 * individually.
 *
 * @param server  Server handle.
 */
extern void xylem_ws_close_server(xylem_ws_server_t* server);
