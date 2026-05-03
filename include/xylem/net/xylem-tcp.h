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
    bool disable_mss_clamp; /*< Disable MSS clamping on the socket. */
} xylem_tcp_opts_t;

typedef struct xylem_tcp_frame_opts_s {
    uint32_t header_size;  /*< Total header size in bytes. */
    uint32_t field_offset; /*< Byte offset of the length field within header. */
    uint32_t field_size;   /*< Size of the length field in bytes (1-8). */
    int32_t  adjustment;   /*< Added to decoded length to get payload size. */
    bool     big_endian;   /*< true: big-endian length field. */
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
    const char* host,
    uint16_t port,
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
 * Suspends the calling coroutine until the connection is established.
 *
 * @param host  Remote hostname or IP address.
 * @param port  Remote port.
 * @param opts  Options, NULL for defaults.
 *
 * @return Connection handle, or NULL on failure.
 */
extern xylem_tcp_conn_t* xylem_tcp_dial(
    const char* host,
    uint16_t port,
    xylem_tcp_opts_t* opts);

/**
 * @brief Connect to a remote TCP endpoint with a timeout.
 *
 * @param host  Remote hostname or IP address.
 * @param port  Remote port.
 * @param opts  Options, NULL for defaults.
 * @param ms    Timeout in milliseconds.
 *
 * @return Connection handle, or NULL on failure or timeout.
 */
extern xylem_tcp_conn_t* xylem_tcp_dial_timeout(
    const char* host,
    uint16_t port,
    xylem_tcp_opts_t* opts,
    uint64_t ms);

/**
 * @brief Receive up to len bytes from the connection.
 *
 * Suspends the calling coroutine until data is available.
 *
 * @param tcp  Connection handle.
 * @param buf  Destination buffer.
 * @param len  Maximum bytes to read.
 *
 * @return Bytes received (>0), 0 on peer close, -1 on error.
 */
extern int64_t xylem_tcp_recv(
    xylem_tcp_conn_t* tcp,
    void* buf,
    size_t len);

/**
 * @brief Receive up to len bytes with a timeout.
 *
 * @param tcp  Connection handle.
 * @param buf  Destination buffer.
 * @param len  Maximum bytes to read.
 * @param ms   Timeout in milliseconds.
 *
 * @return Bytes received (>0), 0 on peer close, -1 on error or timeout.
 */
extern int64_t xylem_tcp_recv_timeout(
    xylem_tcp_conn_t* tcp,
    void* buf,
    size_t len,
    uint64_t ms);

/**
 * @brief Receive exactly len bytes from the connection.
 *
 * Blocks until all bytes are received or an error occurs.
 *
 * @param tcp  Connection handle.
 * @param buf  Destination buffer.
 * @param len  Exact number of bytes to read.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_tcp_recv_exact(
    xylem_tcp_conn_t* tcp,
    void* buf,
    size_t len);

/**
 * @brief Send all bytes to the connection.
 *
 * Blocks until all data is written or an error occurs.
 *
 * @param tcp   Connection handle.
 * @param data  Source buffer.
 * @param len   Number of bytes to send.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_tcp_send(
    xylem_tcp_conn_t* tcp,
    const void* data,
    size_t len);

/**
 * @brief Receive a length-prefixed frame.
 *
 * Reads the header, decodes the length field, then reads the payload.
 * Caller must free() the returned buffer.
 *
 * @param tcp      Connection handle.
 * @param opts     Frame format options.
 * @param out_len  Receives the payload length.
 *
 * @return Allocated payload buffer, or NULL on error.
 */
extern void* xylem_tcp_recv_frame(
    xylem_tcp_conn_t* tcp,
    xylem_tcp_frame_opts_t* opts,
    size_t* out_len);

/**
 * @brief Send a length-prefixed frame.
 *
 * Encodes the length into a header and sends header + payload.
 *
 * @param tcp   Connection handle.
 * @param opts  Frame format options.
 * @param data  Payload buffer.
 * @param len   Payload length.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_tcp_send_frame(
    xylem_tcp_conn_t* tcp,
    xylem_tcp_frame_opts_t* opts,
    const void* data,
    size_t len);

/**
 * @brief Receive a line terminated by LF or CRLF.
 *
 * The terminator is stripped from the result. The buffer is
 * null-terminated.
 *
 * @param tcp  Connection handle.
 * @param buf  Destination buffer.
 * @param max  Buffer size including null terminator.
 *
 * @return Length of the line (excluding terminator), or -1 on error.
 */
extern int64_t xylem_tcp_recv_line(
    xylem_tcp_conn_t* tcp,
    char* buf,
    size_t max);

/**
 * @brief Close and destroy a connection.
 *
 * @param tcp  Connection handle.
 */
extern void xylem_tcp_close(xylem_tcp_conn_t* tcp);

/**
 * @brief Get the last error code from the connection.
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
    char* host,
    size_t host_len,
    uint16_t* port);

/**
 * @brief Get user-associated data from a connection.
 *
 * @param tcp  Connection handle.
 *
 * @return User data pointer, or NULL if not set.
 */
extern void* xylem_tcp_get_userdata(xylem_tcp_conn_t* tcp);

/**
 * @brief Set user-associated data on a connection.
 *
 * @param tcp  Connection handle.
 * @param ud   User data pointer.
 */
extern void xylem_tcp_set_userdata(xylem_tcp_conn_t* tcp, void* ud);

/**
 * @brief Get user-associated data from a listener.
 *
 * @param ln  Listener handle.
 *
 * @return User data pointer, or NULL if not set.
 */
extern void* xylem_tcp_listener_get_userdata(xylem_tcp_listener_t* ln);

/**
 * @brief Set user-associated data on a listener.
 *
 * @param ln  Listener handle.
 * @param ud  User data pointer.
 */
extern void xylem_tcp_listener_set_userdata(
    xylem_tcp_listener_t* ln,
    void* ud);
