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
#include "xylem/net/xylem-tls.h"

typedef struct xylem_wss_listener_s xylem_wss_listener_t;

/**
 * @brief Upgrade an HTTPS request to a WSS connection (server side).
 *
 * @param res   HTTP response handle.
 * @param req   HTTP request handle.
 * @param opts  WebSocket options, or NULL for defaults.
 *
 * @return WebSocket connection, or NULL on failure.
 */
extern xylem_ws_conn_t* xylem_wss_accept(struct xylem_http_res_s* res,
                                          struct xylem_http_req_s* req,
                                          const xylem_ws_opts_t* opts);

/**
 * @brief Start a standalone WSS server with TLS.
 *
 * @param host      Bind address, or NULL for any.
 * @param port      Bind port.
 * @param handler   Connection handler invoked per accepted client.
 * @param userdata  Opaque pointer passed to handler.
 * @param tls_ctx   TLS context for server certificate.
 * @param opts      WebSocket options, or NULL for defaults.
 *
 * @return Listener handle, or NULL on failure.
 */
extern xylem_wss_listener_t* xylem_wss_listen(const char* host, uint16_t port,
                                               xylem_ws_handler_fn_t handler,
                                               void* userdata,
                                               xylem_tls_ctx_t* tls_ctx,
                                               const xylem_ws_opts_t* opts);

/**
 * @brief Close a WSS listener.
 *
 * @param listener  Listener handle.
 */
extern void     xylem_wss_close_listener(xylem_wss_listener_t* listener);

/**
 * @brief Return the port the WSS listener is bound to.
 *
 * @param listener  Listener handle.
 *
 * @return Bound port number.
 */
extern uint16_t xylem_wss_listener_port(xylem_wss_listener_t* listener);

/**
 * @brief Connect to a WSS server.
 *
 * @param url      WSS URL (wss://host:port/path).
 * @param tls_ctx  TLS context for client verification.
 * @param opts     WebSocket options, or NULL for defaults.
 *
 * @return WebSocket connection, or NULL on failure.
 */
extern xylem_ws_conn_t* xylem_wss_dial(const char* url,
                                        xylem_tls_ctx_t* tls_ctx,
                                        const xylem_ws_opts_t* opts);
