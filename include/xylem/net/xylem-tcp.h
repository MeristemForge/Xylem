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

typedef struct xylem_tcp_conn_s     xylem_tcp_conn_t;
typedef struct xylem_tcp_listener_s xylem_tcp_listener_t;

typedef struct xylem_tcp_opts_s {
    size_t max_read_buf;      /*< Internal read buffer size, 0 = default 64KB. */
    bool   disable_mss_clamp; /*< Disable MSS clamping on the socket. */
} xylem_tcp_opts_t;

typedef enum xylem_tcp_frame_type_e {
    XYLEM_TCP_FRAME_NONE,      /*< Raw mode, recv returns available bytes. */
    XYLEM_TCP_FRAME_FIXED,     /*< Fixed-length frames. */
    XYLEM_TCP_FRAME_LENGTH,    /*< Length-prefixed frames. */
    XYLEM_TCP_FRAME_DELIMITER, /*< Delimiter-terminated frames. */
} xylem_tcp_frame_type_t;

typedef struct xylem_tcp_frame_opts_s {
    xylem_tcp_frame_type_t type;
    union {
        struct {
            size_t len; /*< Fixed frame length in bytes. */
        } fixed;
        struct {
            uint32_t header_size;  /*< Total header size in bytes. */
            uint32_t field_offset; /*< Byte offset of the length field. */
            uint32_t field_size;   /*< Size of the length field (1-8). */
            int32_t  adjustment;   /*< Added to decoded length for payload size. */
            bool     big_endian;   /*< true: big-endian length field. */
        } length;
        struct {
            const char* delim;     /*< Delimiter bytes. */
            size_t      delim_len; /*< Delimiter length, 0 = auto strlen. */
        } delimiter;
    };
} xylem_tcp_frame_opts_t;

/**
 * @brief Create a TCP listener bound to the given address.
 *
 * @param host  Bind address (e.g. "0.0.0.0"), or NULL for any.
 * @param port  Bind port.
 * @param opts  Options, NULL for defaults.
 *
 * @return Listener handle, or NULL on failure.
 */
extern xylem_tcp_listener_t* xylem_tcp_listen(
    const char*       host,
    uint16_t          port,
    xylem_tcp_opts_t* opts);

/**
 * @brief Accept a connection from the listener.
 *
 * Suspends the calling coroutine until a client connects.
 *
 * @param ln  Listener handle.
 *
 * @return Accepted connection, or NULL if the listener is closing.
 */
extern xylem_tcp_conn_t* xylem_tcp_accept(xylem_tcp_listener_t* ln);

/**
 * @brief Close and destroy a listener.
 *
 * Wakes any coroutine blocked in xylem_tcp_accept().
 *
 * @param ln  Listener handle.
 */
extern void xylem_tcp_close_listener(xylem_tcp_listener_t* ln);

/**
 * @brief Connect to a remote TCP endpoint.
 *
 * Suspends the calling coroutine until the connection is established
 * or connect_timeout_ms elapses.
 *
 * @param host                Remote hostname or IP address.
 * @param port                Remote port.
 * @param connect_timeout_ms  Connect timeout in ms, 0 = no timeout.
 * @param opts                Options, NULL for defaults.
 *
 * @return Connection handle, or NULL on failure or timeout.
 */
extern xylem_tcp_conn_t* xylem_tcp_dial(
    const char*       host,
    uint16_t          port,
    uint64_t          connect_timeout_ms,
    xylem_tcp_opts_t* opts);

/**
 * @brief Set the framing mode for subsequent recv/send calls.
 *
 * @param tcp   Connection handle.
 * @param opts  Frame options, NULL to reset to raw mode.
 */
extern void xylem_tcp_set_framing(
    xylem_tcp_conn_t*       tcp,
    xylem_tcp_frame_opts_t* opts);

/**
 * @brief Set the read deadline for the connection.
 *
 * Subsequent xylem_tcp_recv() calls return -1 (errno ETIMEDOUT) once
 * the clock passes the deadline, even if partial data has been read.
 * A deadline already-parked xylem_tcp_recv() is also woken up.
 *
 * @param tcp          Connection handle.
 * @param deadline_ms  Monotonic deadline in ms (from
 *                     xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC)),
 *                     or 0 to clear the deadline.
 */
extern void xylem_tcp_set_read_deadline(
    xylem_tcp_conn_t* tcp,
    uint64_t          deadline_ms);

/**
 * @brief Set the write deadline for the connection.
 *
 * Mirror of xylem_tcp_set_read_deadline for the write direction.
 *
 * @param tcp          Connection handle.
 * @param deadline_ms  Monotonic deadline in ms, or 0 to clear.
 */
extern void xylem_tcp_set_write_deadline(
    xylem_tcp_conn_t* tcp,
    uint64_t          deadline_ms);

/**
 * @brief Receive data or a complete frame from the connection.
 *
 * Behavior depends on the configured framing mode:
 *   - NONE:      returns 1~len available bytes (raw read).
 *   - FIXED:     returns exactly frame_opts.fixed.len bytes.
 *   - LENGTH:    reads header, decodes length, returns payload.
 *   - DELIMITER: reads until delimiter, returns data without it.
 *
 * @param tcp  Connection handle.
 * @param buf  Destination buffer.
 * @param len  Buffer size.
 *
 * @return Bytes/frame length (>0), 0 on peer close, -1 on error/timeout.
 */
extern int64_t xylem_tcp_recv(
    xylem_tcp_conn_t* tcp,
    void*             buf,
    size_t            len);

/**
 * @brief Send data or a framed message to the connection.
 *
 * Behavior depends on the configured framing mode:
 *   - NONE/FIXED/DELIMITER: sends data as-is.
 *   - LENGTH: prepends the encoded length header before data.
 *
 * @param tcp   Connection handle.
 * @param data  Source buffer.
 * @param len   Number of bytes to send.
 *
 * @return 0 on success, -1 on error or timeout.
 */
extern int xylem_tcp_send(
    xylem_tcp_conn_t* tcp,
    const void*       data,
    size_t            len);

/**
 * @brief Close and destroy a connection.
 *
 * The connection must not be used after this call. Read any needed
 * state (xylem_tcp_get_error, xylem_tcp_remote_addr) before closing.
 *
 * @param tcp  Connection handle.
 */
extern void xylem_tcp_close(xylem_tcp_conn_t* tcp);

/**
 * @brief Get the last error code from the connection.
 *
 * Must be called before xylem_tcp_close().
 *
 * @param tcp  Connection handle.
 *
 * @return Platform-specific error code, or 0 if no error.
 */
extern int xylem_tcp_get_error(xylem_tcp_conn_t* tcp);

/**
 * @brief Get the remote address of the connection.
 *
 * @param tcp       Connection handle.
 * @param host      Buffer to receive the address string.
 * @param host_len  Size of host buffer (46 bytes recommended).
 * @param port      Receives the remote port.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_tcp_remote_addr(
    xylem_tcp_conn_t* tcp,
    char*             host,
    size_t            host_len,
    uint16_t*         port);
