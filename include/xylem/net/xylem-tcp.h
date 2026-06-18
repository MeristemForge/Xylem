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
    bool enable_mss_clamp; /* Clamp socket MSS to the minimum; default off,
                              so the socket uses the path MTU. */
} xylem_tcp_opts_t;

/**
 * @brief Create a TCP listener bound to the given address.
 *
 * @note [COROUTINE-ONLY]
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
 * @note [COROUTINE-ONLY]
 *
 * Suspends the calling coroutine until a client connects.
 * Call from a single coroutine per listener; a concurrent second
 * accept on the same listener violates the underlying iowait
 * single-waiter contract and aborts.
 *
 * @param ln  Listener handle.
 *
 * @return Accepted connection, or NULL if the listener is closed.
 */
extern xylem_tcp_conn_t* xylem_tcp_accept(xylem_tcp_listener_t* ln);

/**
 * @brief Close and destroy a listener. Idempotent.
 *
 * @note [COROUTINE-ONLY]
 *
 * Wakes any coroutine blocked in xylem_tcp_accept().
 *
 * @param ln  Listener handle.
 */
extern void xylem_tcp_close_listener(xylem_tcp_listener_t* ln);

/**
 * @brief Connect to a remote TCP endpoint.
 *
 * @note [COROUTINE-ONLY]
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
 * @brief Set the read deadline for the connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * Once the clock passes the deadline, in-flight and subsequent
 * xylem_tcp_read() calls return -1.
 *
 * @param tcp          Connection handle.
 * @param deadline_ms  Absolute monotonic timestamp in ms, or 0
 *                     to clear.
 */
extern void xylem_tcp_set_read_deadline(
    xylem_tcp_conn_t* tcp,
    uint64_t          deadline_ms);

/**
 * @brief Set the write deadline for the connection.
 *
 * @note [COROUTINE-ONLY]
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
 * @brief Read data from the connection (read-some semantics).
 *
 * @note [COROUTINE-ONLY]
 *
 * Returns available data from the socket. Suspends the calling
 * coroutine if no data is immediately available. At most len
 * bytes are returned; the actual count may be less.
 * At most one coroutine may be blocked in read on a connection
 * at a time; a concurrent second reader aborts.
 *
 * @param tcp  Connection handle.
 * @param buf  Destination buffer.
 * @param len  Maximum bytes to read.
 *
 * @return Bytes read (>0), 0 on peer close, -1 on error/timeout.
 */
extern int xylem_tcp_read(
    xylem_tcp_conn_t* tcp,
    void*             buf,
    int               len);

/**
 * @brief Write all data to the connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * Loops internally until all len bytes are sent or an error
 * occurs. Suspends the calling coroutine as needed.
 * At most one coroutine may be blocked in write on a connection
 * at a time; a concurrent second writer aborts.
 *
 * @param tcp   Connection handle.
 * @param data  Source buffer.
 * @param len   Number of bytes to write.
 *
 * @return 0 on success, -1 on error or timeout.
 */
extern int xylem_tcp_write(
    xylem_tcp_conn_t* tcp,
    const void*       data,
    int               len);

/**
 * @brief Close a connection. Idempotent.
 *
 * @note [COROUTINE-ONLY]
 *
 * Wakes any coroutine blocked in read/write. Read any needed state
 * (xylem_tcp_remote_addr) before closing.
 *
 * @param tcp  Connection handle.
 */
extern void xylem_tcp_close(xylem_tcp_conn_t* tcp);

/**
 * @brief Get the remote address of the connection.
 *
 * @note [COROUTINE-ONLY]
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

/**
 * @brief Get the local address of the connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param tcp       Connection handle.
 * @param host      Buffer to receive the address string.
 * @param host_len  Size of host buffer (46 bytes recommended).
 * @param port      Receives the local port.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_tcp_local_addr(
    xylem_tcp_conn_t* tcp,
    char*             host,
    size_t            host_len,
    uint16_t*         port);

/**
 * @brief Get the local address of the listener.
 *
 * @note [COROUTINE-ONLY]
 *
 * Useful after binding to port 0 to discover the assigned port.
 *
 * @param ln        Listener handle.
 * @param host      Buffer to receive the address string.
 * @param host_len  Size of host buffer (46 bytes recommended).
 * @param port      Receives the local port.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_tcp_listener_addr(
    xylem_tcp_listener_t* ln,
    char*                 host,
    size_t                host_len,
    uint16_t*             port);

/**
 * @brief Shut down the write side of the connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * Sends a FIN to the peer, signalling that no more data will be
 * written. The connection remains readable; the peer sees EOF on
 * their next read. Use this for graceful half-close protocols.
 *
 * @param tcp  Connection handle.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_tcp_shutdown_wr(xylem_tcp_conn_t* tcp);

/**
 * @brief Shut down the read side of the connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * Discards further incoming data. Subsequent read calls return -1.
 *
 * @param tcp  Connection handle.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_tcp_shutdown_rd(xylem_tcp_conn_t* tcp);
