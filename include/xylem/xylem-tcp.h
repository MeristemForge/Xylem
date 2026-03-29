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

typedef enum xylem_tcp_framing_type_e {
    XYLEM_TCP_FRAME_NONE,
    XYLEM_TCP_FRAME_FIXED,
    XYLEM_TCP_FRAME_LENGTH,
    XYLEM_TCP_FRAME_DELIM,
    XYLEM_TCP_FRAME_CUSTOM,
} xylem_tcp_framing_type_t;

typedef enum xylem_tcp_timeout_type_e {
    XYLEM_TCP_TIMEOUT_READ,
    XYLEM_TCP_TIMEOUT_WRITE,
    XYLEM_TCP_TIMEOUT_CONNECT,
} xylem_tcp_timeout_type_t;

typedef enum xylem_tcp_length_coding_e {
    XYLEM_TCP_LENGTH_FIXEDINT,
    XYLEM_TCP_LENGTH_VARINT,
} xylem_tcp_length_coding_t;

typedef struct xylem_tcp_framing_s {
    xylem_tcp_framing_type_t type;
    union {
        struct { size_t frame_size; }                          fixed;
        struct {
            uint32_t                  header_size;   /**< Fixed header size (payload starts after). */
            uint32_t                  field_offset;  /**< Length field offset in header. */
            uint32_t                  field_size;    /**< Length field size in bytes. */
            int32_t                   adjustment;    /**< Length adjustment value. */
            xylem_tcp_length_coding_t coding;        /**< FIXEDINT or VARINT. */
            bool                      field_big_endian; /**< Byte order (FIXEDINT only). */
        } length;
        struct { const char* delim; size_t delim_len; }        delim;
        struct { int (*parse)(const void* data, size_t len); } custom;
    };
} xylem_tcp_framing_t;

typedef struct xylem_tcp_conn_s   xylem_tcp_conn_t;
typedef struct xylem_tcp_server_s xylem_tcp_server_t;

typedef struct xylem_tcp_handler_s {
    void (*on_connect)(xylem_tcp_conn_t* conn);
    void (*on_accept)(xylem_tcp_server_t* server, xylem_tcp_conn_t* conn);
    void (*on_read)(xylem_tcp_conn_t* conn, void* data, size_t len);
    void (*on_write_done)(xylem_tcp_conn_t* conn,
                          void* data, size_t len, int status);
    void (*on_timeout)(xylem_tcp_conn_t* conn,
                       xylem_tcp_timeout_type_t type);
    void (*on_close)(xylem_tcp_conn_t* conn, int err);
    void (*on_heartbeat_miss)(xylem_tcp_conn_t* conn);
} xylem_tcp_handler_t;

typedef struct xylem_tcp_opts_s {
    xylem_tcp_framing_t framing;
    uint64_t connect_timeout_ms;
    uint64_t read_timeout_ms;
    uint64_t write_timeout_ms;
    uint64_t heartbeat_ms;
    uint32_t reconnect_max;
    size_t   read_buf_size;
} xylem_tcp_opts_t;

/**
 * @brief Create a TCP server and start listening.
 *
 * Binds to the specified address, sets non-blocking mode, and
 * registers with the event loop. Calls handler->on_accept when
 * a new connection arrives.
 *
 * @param loop     Event loop.
 * @param addr     Bind address.
 * @param handler  Event callback set.
 * @param opts     TCP options, NULL for defaults.
 *
 * @return Server handle, or NULL on failure.
 */
extern xylem_tcp_server_t* xylem_tcp_listen(xylem_loop_t* loop,
                                            xylem_addr_t* addr,
                                            xylem_tcp_handler_t* handler,
                                            xylem_tcp_opts_t* opts);

/**
 * @brief Close a TCP server.
 *
 * Stops accepting new connections. Existing connections are
 * not affected and must be closed individually.
 *
 * @param server  Server handle.
 */
extern void xylem_tcp_close_server(xylem_tcp_server_t* server);

/**
 * @brief Initiate an asynchronous TCP connection.
 *
 * Creates a non-blocking socket and starts connecting. Calls
 * handler->on_connect on success, or retries/closes on failure
 * depending on opts.
 *
 * @param loop     Event loop.
 * @param addr     Target address (already resolved).
 * @param handler  Event callback set.
 * @param opts     TCP options, NULL for defaults.
 *
 * @return Connection handle, or NULL on failure.
 */
extern xylem_tcp_conn_t* xylem_tcp_dial(xylem_loop_t* loop,
                                        xylem_addr_t* addr,
                                        xylem_tcp_handler_t* handler,
                                        xylem_tcp_opts_t* opts);

/**
 * @brief Send data over a TCP connection.
 *
 * Data is copied into an internal write queue and returns
 * immediately. Each write request triggers handler->on_write_done
 * upon completion.
 *
 * @param conn  Connection handle.
 * @param data  Data to send.
 * @param len   Data length in bytes.
 *
 * @return 0 on success (enqueued), -1 on failure (connection closed).
 *
 * @note Not thread-safe. Must be called from the loop thread.
 *       Use xylem_loop_post() to send from other threads.
 */
extern int xylem_tcp_send(xylem_tcp_conn_t* conn,
                          const void* data, size_t len);

/**
 * @brief Close a TCP connection.
 *
 * Performs a graceful shutdown: flushes the write queue, then
 * calls shutdown + close. Calls handler->on_close when done.
 *
 * @param conn  Connection handle.
 *
 * @note Not thread-safe. Must be called from the loop thread.
 */
extern void xylem_tcp_close(xylem_tcp_conn_t* conn);

/**
 * @brief Get the peer address of a connection.
 *
 * Returns a pointer to the peer address stored at accept or
 * connect time. The pointer is valid for the lifetime of the
 * connection.
 *
 * @param conn  Connection handle.
 *
 * @return Peer address, or NULL if not available.
 */
extern const xylem_addr_t* xylem_tcp_get_peer_addr(xylem_tcp_conn_t* conn);

/**
 * @brief Get user data attached to a connection.
 *
 * @param conn  Connection handle.
 *
 * @return User data pointer.
 */
extern void* xylem_tcp_get_userdata(xylem_tcp_conn_t* conn);

/**
 * @brief Set user data on a connection.
 *
 * @param conn  Connection handle.
 * @param ud    User data pointer.
 */
extern void xylem_tcp_set_userdata(xylem_tcp_conn_t* conn, void* ud);

/**
 * @brief Get user data attached to a TCP server.
 *
 * @param server  Server handle.
 *
 * @return User data pointer.
 */
extern void* xylem_tcp_server_get_userdata(xylem_tcp_server_t* server);

/**
 * @brief Set user data on a TCP server.
 *
 * @param server  Server handle.
 * @param ud      User data pointer.
 */
extern void xylem_tcp_server_set_userdata(xylem_tcp_server_t* server,
                                          void* ud);
