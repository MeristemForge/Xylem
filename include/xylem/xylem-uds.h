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

#include "xylem/xylem-loop.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Framing strategy for UDS stream reassembly. */
typedef enum xylem_uds_framing_type_e {
    XYLEM_UDS_FRAME_NONE,   /**< No framing, raw byte stream. */
    XYLEM_UDS_FRAME_FIXED,  /**< Fixed-length frames. */
    XYLEM_UDS_FRAME_LENGTH, /**< Length-prefixed frames. */
    XYLEM_UDS_FRAME_DELIM,  /**< Delimiter-separated frames. */
    XYLEM_UDS_FRAME_CUSTOM, /**< User-supplied parse function. */
} xylem_uds_framing_type_t;

/** Timeout category reported via on_timeout. */
typedef enum xylem_uds_timeout_type_e {
    XYLEM_UDS_TIMEOUT_READ,  /**< Read idle timeout. */
    XYLEM_UDS_TIMEOUT_WRITE, /**< Write completion timeout. */
} xylem_uds_timeout_type_t;

/** Encoding used for the length field in LENGTH framing. */
typedef enum xylem_uds_length_coding_e {
    XYLEM_UDS_LENGTH_FIXEDINT, /**< Fixed-width integer (1-8 bytes). */
    XYLEM_UDS_LENGTH_VARINT,   /**< Variable-length integer. */
} xylem_uds_length_coding_t;

/** Framing configuration for UDS connections. */
typedef struct xylem_uds_framing_s {
    xylem_uds_framing_type_t type;
    union {
        struct { size_t frame_size; }                          fixed;
        struct {
            uint32_t                  header_size;   /**< Fixed header size. */
            uint32_t                  field_offset;  /**< Length field offset. */
            uint32_t                  field_size;    /**< Length field size. */
            int32_t                   adjustment;    /**< Length adjustment. */
            xylem_uds_length_coding_t coding;        /**< FIXEDINT or VARINT. */
            bool                      field_big_endian; /**< Byte order. */
        } length;
        struct { const char* delim; size_t delim_len; }        delim;
        struct { int (*parse)(const void* data, size_t len); } custom;
    };
} xylem_uds_framing_t;

/** Opaque UDS connection handle. */
typedef struct xylem_uds_conn_s   xylem_uds_conn_t;
/** Opaque UDS server handle. */
typedef struct xylem_uds_server_s xylem_uds_server_t;

/** UDS event callback set. */
typedef struct xylem_uds_handler_s {
    void (*on_connect)(xylem_uds_conn_t* conn);           /**< Client connected. */
    void (*on_accept)(xylem_uds_server_t* server,
                      xylem_uds_conn_t* conn);             /**< Server accepted. */
    void (*on_read)(xylem_uds_conn_t* conn,
                    void* data, size_t len);                /**< Frame received. */
    void (*on_write_done)(xylem_uds_conn_t* conn,
                          const void* data, size_t len,
                          int status);                      /**< Write completed. */
    void (*on_timeout)(xylem_uds_conn_t* conn,
                       xylem_uds_timeout_type_t type);      /**< Timeout fired. */
    void (*on_close)(xylem_uds_conn_t* conn,
                     int err, const char* errmsg);          /**< Connection closed. */
    void (*on_heartbeat_miss)(xylem_uds_conn_t* conn);     /**< No data received within heartbeat interval. */
} xylem_uds_handler_t;

/** UDS connection options. */
typedef struct xylem_uds_opts_s {
    xylem_uds_framing_t framing;            /**< Framing configuration. */
    uint64_t            read_timeout_ms;    /**< Read idle timeout, 0 = none. */
    uint64_t            write_timeout_ms;   /**< Write timeout, 0 = none. */
    uint64_t            heartbeat_ms;       /**< Heartbeat interval, 0 = none. */
    size_t              read_buf_size;      /**< Read buffer size, 0 = 65536. */
} xylem_uds_opts_t;

/**
 * @brief Create a UDS server and start listening.
 *
 * Binds to the specified filesystem path, sets non-blocking mode,
 * and registers with the event loop. Unlinks the path first if it
 * already exists. Calls handler->on_accept for new connections.
 *
 * @param loop     Event loop.
 * @param path     Unix domain socket path.
 * @param handler  Event callback set.
 * @param opts     UDS options, NULL for defaults.
 *
 * @return Server handle, or NULL on failure.
 */
extern xylem_uds_server_t* xylem_uds_listen(xylem_loop_t* loop,
                                             const char* path,
                                             xylem_uds_handler_t* handler,
                                             xylem_uds_opts_t* opts);

/**
 * @brief Close a UDS server.
 *
 * Stops accepting new connections, closes all existing connections,
 * and unlinks the socket file.
 *
 * @param server  Server handle.
 */
extern void xylem_uds_close_server(xylem_uds_server_t* server);

/**
 * @brief Connect to a Unix domain socket.
 *
 * Creates a non-blocking AF_UNIX SOCK_STREAM socket and connects
 * to the specified path. Calls handler->on_connect on success.
 *
 * @param loop     Event loop.
 * @param path     Unix domain socket path.
 * @param handler  Event callback set.
 * @param opts     UDS options, NULL for defaults.
 *
 * @return Connection handle, or NULL on failure.
 */
extern xylem_uds_conn_t* xylem_uds_dial(xylem_loop_t* loop,
                                         const char* path,
                                         xylem_uds_handler_t* handler,
                                         xylem_uds_opts_t* opts);

/**
 * @brief Send data over a UDS connection.
 *
 * Data is copied into an internal write queue and returns
 * immediately. Each write triggers handler->on_write_done.
 *
 * @param conn  Connection handle.
 * @param data  Data to send.
 * @param len   Data length in bytes.
 *
 * @return 0 on success (enqueued), -1 on failure.
 */
extern int xylem_uds_send(xylem_uds_conn_t* conn,
                          const void* data, size_t len);

/**
 * @brief Close a UDS connection.
 *
 * Flushes the write queue, then shuts down and closes the socket.
 * Calls handler->on_close when done.
 *
 * @param conn  Connection handle.
 */
extern void xylem_uds_close(xylem_uds_conn_t* conn);

/**
 * @brief Get the event loop associated with a connection.
 *
 * @param conn  Connection handle.
 *
 * @return Loop handle.
 */
extern xylem_loop_t* xylem_uds_get_loop(xylem_uds_conn_t* conn);

/**
 * @brief Get user data attached to a connection.
 *
 * @param conn  Connection handle.
 *
 * @return User data pointer.
 */
extern void* xylem_uds_get_userdata(xylem_uds_conn_t* conn);

/**
 * @brief Set user data on a connection.
 *
 * @param conn  Connection handle.
 * @param ud    User data pointer.
 */
extern void xylem_uds_set_userdata(xylem_uds_conn_t* conn, void* ud);

/**
 * @brief Get user data attached to a UDS server.
 *
 * @param server  Server handle.
 *
 * @return User data pointer.
 */
extern void* xylem_uds_server_get_userdata(xylem_uds_server_t* server);

/**
 * @brief Set user data on a UDS server.
 *
 * @param server  Server handle.
 * @param ud      User data pointer.
 */
extern void xylem_uds_server_set_userdata(xylem_uds_server_t* server,
                                          void* ud);
