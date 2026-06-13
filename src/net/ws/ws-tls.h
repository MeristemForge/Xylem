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
 * Secure WebSocket (wss) dial factory.
 *
 * Establishes a TLS client connection and runs the WebSocket handshake
 * over it. The real implementation lives in ws-tls.c when XYLEM_ENABLE_TLS
 * is set; otherwise ws-tls-stub.c provides a NULL-returning stub, so the
 * unconditionally-built public dial (xylem-ws.c) degrades to "wss not
 * available" with no TLS symbols leaking into the engine. The mirror of
 * ws-tcp.h. Not part of the public API.
 */

_Pragma("once")

#include "ws.h"

/**
 * @brief Dial a secure WebSocket (wss) server and complete the handshake.
 *
 * @param host  Server host (also used as TLS SNI).
 * @param port  Server port.
 * @param path  Request path (e.g. "/").
 * @param tls   TLS configuration (CA, client cert, skip_verify), or NULL.
 * @param opts  WebSocket options, or NULL for defaults.
 *
 * @return Connection handle, or NULL on failure (or always NULL in the
 *         stub build without TLS).
 */
extern xylem_ws_conn_t* ws_tls_dial(const char* host, uint16_t port,
                                     const char* path,
                                     const xylem_ws_tls_t* tls,
                                     const xylem_ws_opts_t* opts);
