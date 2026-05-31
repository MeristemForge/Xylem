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

#include "net/http/http-internal.h"
#include "ws-deflate.h"
#include "xylem/net/xylem-ws.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_DEFAULT_MAX_MSG_SIZE       (16 * 1024 * 1024)
#define WS_DEFAULT_FRAGMENT_THRESHOLD (16 * 1024)
#define WS_DEFAULT_HANDSHAKE_TIMEOUT  10000
#define WS_DEFAULT_CLOSE_TIMEOUT      5000
#define WS_RECV_BUF_INIT              4096

struct xylem_ws_conn_s {
    http_transport_t  transport;
    bool              is_client;
    bool              _standalone;
    uint8_t*          recv_buf;
    size_t            recv_len;
    size_t            recv_cap;
    uint8_t*          frag_buf;
    size_t            frag_len;
    size_t            frag_cap;
    uint8_t           frag_opcode;
    bool              frag_active;
    size_t            max_msg_size;
    size_t            fragment_threshold;
    uint64_t          close_timeout_ms;
    uint16_t          close_code;
    bool              close_sent;
    bool              close_received;
    void*             userdata;
    ws_deflate_ctx_t  deflate_ctx;
    bool              deflate_requested;
    bool              deflate_context_takeover;
    bool              frag_compressed;
};

extern xylem_ws_conn_t* ws_accept_impl(struct xylem_http_res_s* res,
                                       struct xylem_http_req_s* req,
                                       const xylem_ws_opts_t* opts);

extern xylem_ws_conn_t* ws_conn_create(http_transport_t transport,
                                        bool is_client,
                                        const xylem_ws_opts_t* opts);

extern void ws_conn_free(xylem_ws_conn_t* conn);

extern xylem_ws_conn_t* ws_dial_impl(http_transport_t transport,
                                      const char* host, uint16_t port,
                                      const char* path,
                                      const xylem_ws_opts_t* opts);
