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
 * Plain WebSocket (ws) dial factory.
 *
 * Establishes a plain-TCP client connection and runs the WebSocket
 * handshake over it (ws-tcp.c, always built). The mirror of ws-tls.h,
 * which declares the secure (wss) dial factory. xylem_ws_dial() parses
 * the URL scheme and dispatches here for ws:// or to ws_tls_dial() for
 * wss://. Not part of the public API.
 */

_Pragma("once")

#include "ws.h"

/**
 * @brief Dial a plain WebSocket (ws) server and complete the handshake.
 *
 * @param host  Server host.
 * @param port  Server port.
 * @param path  Request path (e.g. "/").
 * @param opts  WebSocket options, or NULL for defaults.
 *
 * @return Connection handle, or NULL on failure.
 */
extern xylem_ws_conn_t* ws_tcp_dial(const char* host, uint16_t port,
                                     const char* path,
                                     const xylem_ws_opts_t* opts);
