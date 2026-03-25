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

#include "ws-transport.h"

#include "llhttp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WS_DEFAULT_FRAGMENT_THRESHOLD  16384
#define WS_DEFAULT_MAX_MESSAGE_SIZE    16777216
#define WS_DEFAULT_HANDSHAKE_TIMEOUT   10000
#define WS_DEFAULT_CLOSE_TIMEOUT       5000
#define WS_INITIAL_RECV_CAP            4096
#define WS_MAX_HEADER_SIZE             14

/* Control frame opcodes (internal use only, not exposed in public API). */
#define WS_OPCODE_CLOSE  0x8
#define WS_OPCODE_PING   0x9
#define WS_OPCODE_PONG   0xA

struct xylem_ws_conn_s {
    const ws_transport_vt_t* vt;
    void*                    transport;
    xylem_loop_t*            loop;

    xylem_ws_handler_t*      handler;
    void*                    userdata;

    xylem_ws_state_t         state;
    bool                     is_client;

    size_t                   max_message_size;
    size_t                   fragment_threshold;
    uint64_t                 close_timeout_ms;

    uint8_t*                 recv_buf;
    size_t                   recv_len;
    size_t                   recv_cap;

    uint8_t*                 frag_buf;
    size_t                   frag_len;
    size_t                   frag_cap;
    uint8_t                  frag_opcode;
    bool                     frag_active;

    char                     handshake_key[32];

    llhttp_t                 http_parser;
    llhttp_settings_t        http_settings;
    char                     accept_value[32];
    size_t                   accept_value_len;
    bool                     handshake_complete;
    bool                     got_upgrade;
    bool                     got_connection;
    bool                     parsing_accept_header;

    xylem_loop_timer_t       handshake_timer;
    xylem_loop_timer_t       close_timer;

    /* Current header field tracking for llhttp */
    const char*              current_header_field;
    size_t                   current_header_field_len;

    uint16_t                 close_code;
    bool                     close_sent;
    bool                     close_received;

    ws_transport_cb_t        transport_cb;

    xylem_ws_server_t*       server;

    char*                    host;
    uint16_t                 port;
    char*                    path;

    /* Server-side handshake: Sec-WebSocket-Key from client request */
    char                     client_ws_key[32];
    size_t                   client_ws_key_len;
    bool                     got_ws_version;
    bool                     got_ws_key;
    bool                     version_ok;
};

/**
 * @brief Case-insensitive memory comparison.
 *
 * Compares len bytes of a and b, treating ASCII uppercase and
 * lowercase letters as equal.
 *
 * @param a    First buffer.
 * @param b    Second buffer.
 * @param len  Number of bytes to compare.
 *
 * @return true if equal (ignoring case), false otherwise.
 */
extern bool ws_memeqi(const char* a, const char* b, size_t len);

/**
 * @brief Allocate and initialize a WebSocket connection.
 *
 * Sets up receive buffers, timers, and default options. The caller
 * must set transport, vt, handler, and server fields after creation.
 *
 * @param loop  Event loop.
 * @param opts  Connection options, NULL for defaults.
 *
 * @return Allocated connection, or NULL on failure.
 */
extern xylem_ws_conn_t* ws_conn_create(xylem_loop_t* loop,
                                       const xylem_ws_opts_t* opts);

/**
 * @brief Destroy a connection and free all internal buffers.
 *
 * @param conn  Connection to destroy.
 */
extern void ws_conn_destroy(xylem_ws_conn_t* conn);

/**
 * @brief Fire the on_close callback and destroy the connection.
 *
 * @param conn        Connection.
 * @param code        Close status code.
 * @param reason      Reason string (may be NULL).
 * @param reason_len  Reason string length.
 */
extern void ws_conn_fire_close(xylem_ws_conn_t* conn, uint16_t code,
                               const char* reason, size_t reason_len);

/**
 * @brief Process received WebSocket frames from the receive buffer.
 *
 * @param conn  Connection with data in recv_buf.
 */
extern void ws_conn_process_recv(xylem_ws_conn_t* conn);

/**
 * @brief Grow the receive buffer to accommodate at least needed bytes.
 *
 * @param conn    Connection.
 * @param needed  Minimum required capacity.
 *
 * @return 0 on success, -1 on allocation failure.
 */
extern int ws_conn_recv_buf_grow(xylem_ws_conn_t* conn, size_t needed);

/**
 * @brief Append data to the fragmentation reassembly buffer.
 *
 * Grows the buffer as needed.
 *
 * @param conn  Connection.
 * @param data  Data to append.
 * @param len   Length of data in bytes.
 *
 * @return 0 on success, -1 on allocation failure.
 */
extern int ws_conn_frag_buf_append(xylem_ws_conn_t* conn,
                                   const uint8_t* data, size_t len);

/**
 * @brief Send a WebSocket frame.
 *
 * Encodes the header, optionally masks the payload (for client
 * connections), and writes the frame to the transport.
 *
 * @param conn    Connection.
 * @param fin     FIN bit value.
 * @param opcode  Frame opcode.
 * @param data    Payload data (may be NULL if len is 0).
 * @param len     Payload length in bytes.
 *
 * @return 0 on success, -1 on failure.
 */
extern int ws_conn_send_frame(xylem_ws_conn_t* conn, bool fin,
                              uint8_t opcode, const void* data, size_t len);

/**
 * @brief Send a close frame with the given code and reason.
 *
 * @param conn        Connection.
 * @param code        Close status code.
 * @param reason      Reason string (may be NULL).
 * @param reason_len  Reason string length.
 */
extern void ws_conn_send_close_frame(xylem_ws_conn_t* conn, uint16_t code,
                                     const char* reason, size_t reason_len);

/**
 * @brief Close timeout callback for the close handshake timer.
 *
 * Forces transport shutdown when the peer does not respond to a
 * close frame within the configured timeout.
 *
 * @param loop   Event loop.
 * @param timer  Timer that fired (embedded in the connection).
 */
extern void ws_conn_close_timeout_cb(xylem_loop_t* loop,
                                     xylem_loop_timer_t* timer);

/**
 * @brief Send a protocol error close frame and begin shutdown.
 *
 * If the connection is OPEN, sends a close frame with the given
 * code and starts the close timeout. If already CLOSING, forces
 * transport shutdown.
 *
 * @param conn  Connection.
 * @param code  Close status code (e.g. 1002, 1007, 1009).
 */
extern void ws_conn_protocol_error(xylem_ws_conn_t* conn, uint16_t code);

/**
 * @brief Handle an incoming close frame.
 *
 * Decodes the close payload, validates the status code and reason
 * UTF-8, and performs the appropriate close handshake response.
 *
 * @param conn     Connection.
 * @param payload  Close frame payload bytes.
 * @param len      Payload length in bytes.
 */
extern void ws_conn_handle_close_frame(xylem_ws_conn_t* conn,
                                       const uint8_t* payload, size_t len);

/**
 * @brief Deliver a complete message to the application handler.
 *
 * Validates UTF-8 for text messages before invoking the on_message
 * callback.
 *
 * @param conn    Connection.
 * @param opcode  Message opcode (text or binary).
 * @param data    Message payload.
 * @param len     Payload length in bytes.
 */
extern void ws_conn_deliver_message(xylem_ws_conn_t* conn, uint8_t opcode,
                                    const uint8_t* data, size_t len);
