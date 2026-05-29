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

typedef enum {
    XYLEM_WS_TEXT   = 0x1,
    XYLEM_WS_BINARY = 0x2,
} xylem_ws_opcode_t;

typedef struct {
    xylem_ws_opcode_t opcode;
    void*             data;
    size_t            len;
} xylem_ws_msg_t;

typedef void (*xylem_ws_handler_fn_t)(xylem_ws_conn_t* conn, void* userdata);

typedef struct {
    size_t   max_msg_size;
    size_t   fragment_threshold;
    uint64_t handshake_timeout_ms;
    uint64_t close_timeout_ms;
    bool     permessage_deflate;
    bool     deflate_context_takeover;
} xylem_ws_opts_t;

/* --- Server: HTTP upgrade path --- */

struct xylem_http_res_s;
struct xylem_http_req_s;

extern xylem_ws_conn_t* xylem_ws_accept(struct xylem_http_res_s* res,
                                         struct xylem_http_req_s* req,
                                         const xylem_ws_opts_t* opts);

/* --- Server: standalone listener --- */

extern xylem_ws_listener_t* xylem_ws_listen(const char* host, uint16_t port,
                                             xylem_ws_handler_fn_t handler,
                                             void* userdata,
                                             const xylem_ws_opts_t* opts);
extern void     xylem_ws_close_listener(xylem_ws_listener_t* listener);
extern uint16_t xylem_ws_listener_port(xylem_ws_listener_t* listener);

/* --- Client --- */

extern xylem_ws_conn_t* xylem_ws_dial(const char* url,
                                       const xylem_ws_opts_t* opts);

/* --- Connection operations --- */

extern int  xylem_ws_send(xylem_ws_conn_t* conn, xylem_ws_opcode_t opcode,
                          const void* data, size_t len);
extern int  xylem_ws_recv(xylem_ws_conn_t* conn, xylem_ws_msg_t* msg);
extern int  xylem_ws_ping(xylem_ws_conn_t* conn, const void* data, size_t len);
extern int  xylem_ws_close(xylem_ws_conn_t* conn, uint16_t code,
                           const char* reason, size_t reason_len);
extern void xylem_ws_msg_free(xylem_ws_msg_t* msg);

/* --- Utilities --- */

extern uint16_t xylem_ws_close_code(xylem_ws_conn_t* conn);
extern void*    xylem_ws_get_userdata(xylem_ws_conn_t* conn);
extern void     xylem_ws_set_userdata(xylem_ws_conn_t* conn, void* ud);
