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

/**
 * @brief Initiate an asynchronous WebSocket connection.
 *
 * Parses the URL scheme to select the transport: ws:// uses TCP,
 * wss:// uses TLS (requires XYLEM_ENABLE_TLS). Performs the TCP/TLS
 * connection followed by the HTTP Upgrade handshake. On success,
 * handler->on_open is invoked.
 *
 * @param loop     Event loop.
 * @param url      WebSocket URL (ws:// or wss://).
 * @param handler  Event callback set.
 * @param opts     Connection options, or NULL for defaults.
 *
 * @return Connection handle, or NULL on failure.
 */
extern xylem_ws_conn_t* xylem_ws_dial(xylem_loop_t* loop,
                                      const char* url,
                                      xylem_ws_handler_t* handler,
                                      xylem_ws_opts_t* opts);

/**
 * @brief Send a WebSocket message.
 *
 * Constructs one or more WebSocket frames from the payload. When the
 * payload exceeds the configured fragment threshold, the message is
 * automatically split into fragments. Client connections apply a
 * random masking key to each frame.
 *
 * @param conn    Connection handle.
 * @param opcode  Message type (XYLEM_WS_OPCODE_TEXT or XYLEM_WS_OPCODE_BINARY).
 * @param data    Payload data.
 * @param len     Payload length in bytes.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_ws_send(xylem_ws_conn_t* conn,
                         xylem_ws_opcode_t opcode,
                         const void* data, size_t len);

/**
 * @brief Send a WebSocket ping frame.
 *
 * @param conn  Connection handle.
 * @param data  Optional ping payload (up to 125 bytes), or NULL.
 * @param len   Payload length in bytes.
 *
 * @return 0 on success, -1 on error (e.g. payload > 125 bytes).
 */
extern int xylem_ws_ping(xylem_ws_conn_t* conn,
                         const void* data, size_t len);

/**
 * @brief Initiate a WebSocket close handshake.
 *
 * Sends a close frame with the specified status code and optional
 * reason string, then transitions to the CLOSING state.
 *
 * @param conn        Connection handle.
 * @param code        Close status code (1000-1003, 1007-1011, 3000-4999).
 * @param reason      Optional UTF-8 reason string, or NULL.
 * @param reason_len  Reason string length in bytes (max 123).
 *
 * @return 0 on success, -1 on error (e.g. invalid status code).
 */
extern int xylem_ws_close(xylem_ws_conn_t* conn,
                          uint16_t code, const char* reason, size_t reason_len);

/**
 * @brief Get user data attached to a WebSocket connection.
 *
 * @param conn  Connection handle.
 *
 * @return User data pointer.
 */
extern void* xylem_ws_get_userdata(xylem_ws_conn_t* conn);

/**
 * @brief Set user data on a WebSocket connection.
 *
 * @param conn  Connection handle.
 * @param ud    User data pointer.
 */
extern void xylem_ws_set_userdata(xylem_ws_conn_t* conn, void* ud);
