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

#include <stddef.h>
#include <stdint.h>

/* WebSocket opcode values (RFC 6455 section 5.2). */
typedef enum xylem_ws_opcode_e {
    XYLEM_WS_OPCODE_TEXT   = 0x1,
    XYLEM_WS_OPCODE_BINARY = 0x2,
} xylem_ws_opcode_t;

/* WebSocket connection state. */
typedef enum xylem_ws_state_e {
    XYLEM_WS_STATE_CONNECTING,
    XYLEM_WS_STATE_OPEN,
    XYLEM_WS_STATE_CLOSING,
    XYLEM_WS_STATE_CLOSED,
} xylem_ws_state_t;

typedef struct xylem_ws_conn_s   xylem_ws_conn_t;
typedef struct xylem_ws_server_s xylem_ws_server_t;

/* WebSocket event handler callbacks. */
typedef struct xylem_ws_handler_s {
    void (*on_open)(xylem_ws_conn_t* conn);
    void (*on_accept)(xylem_ws_conn_t* conn);
    void (*on_message)(xylem_ws_conn_t* conn,
                       xylem_ws_opcode_t opcode,
                       const void* data, size_t len);
    void (*on_ping)(xylem_ws_conn_t* conn,
                    const void* data, size_t len);
    void (*on_pong)(xylem_ws_conn_t* conn,
                    const void* data, size_t len);
    void (*on_close)(xylem_ws_conn_t* conn,
                     uint16_t code, const char* reason, size_t reason_len);
} xylem_ws_handler_t;

/* WebSocket connection options. */
typedef struct xylem_ws_opts_s {
    size_t   max_message_size;      /*< Max incoming message size, 0 = no limit, default 16 MiB. */
    size_t   fragment_threshold;    /*< Send fragmentation threshold, 0 = default 16 KiB. */
    uint64_t handshake_timeout_ms;  /*< Handshake timeout, 0 = default 10000 ms. */
    uint64_t close_timeout_ms;      /*< Close handshake timeout, 0 = default 5000 ms. */
} xylem_ws_opts_t;
