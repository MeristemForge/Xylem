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

typedef struct xylem_rudp_conn_s     xylem_rudp_conn_t;
typedef struct xylem_rudp_listener_s xylem_rudp_listener_t;

typedef enum xylem_rudp_mode_e {
    XYLEM_RUDP_STREAM,  /* Byte stream, compatible with mux/reader/writer. */
    XYLEM_RUDP_MESSAGE  /* Preserves message boundaries. */
} xylem_rudp_mode_t;

typedef struct xylem_rudp_opts_s {
    xylem_rudp_mode_t mode;              /* Stream or message mode. */
    uint32_t          mtu;               /* UDP MTU, 0 = 1400. */
    uint32_t          fec_data;          /* FEC data shards, 0 = disabled. */
    uint32_t          fec_parity;        /* FEC parity shards, 0 = disabled. */
    uint64_t          connect_timeout_ms; /* Handshake timeout, 0 = 5000ms. */
    uint64_t          timeout_ms;        /* Dead link timeout, 0 = default. */
    const uint8_t*    aes_key;           /* 32-byte AES-256 key, NULL = disabled. */
} xylem_rudp_opts_t;

/**
 * @brief Dial a reliable UDP connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * Suspends the calling coroutine until the handshake completes
 * or times out.
 *
 * @param host  Target address string.
 * @param port  Target port.
 * @param opts  Options, or NULL for defaults.
 *
 * @return Connection handle, or NULL on failure.
 */
extern xylem_rudp_conn_t* xylem_rudp_dial(
    const char*        host,
    uint16_t           port,
    xylem_rudp_opts_t* opts);

/**
 * @brief Start listening for reliable UDP connections.
 *
 * @note [COROUTINE-ONLY]
 *
 * Spawns a background dispatcher coroutine.
 *
 * @param host  Bind address string.
 * @param port  Bind port.
 * @param opts  Options, or NULL for defaults.
 *
 * @return Listener handle, or NULL on failure.
 */
extern xylem_rudp_listener_t* xylem_rudp_listen(
    const char*        host,
    uint16_t           port,
    xylem_rudp_opts_t* opts);

/**
 * @brief Accept an incoming RUDP connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * Suspends the calling coroutine until a new session arrives
 * or the listener closes.
 *
 * @param ln  Listener handle.
 *
 * @return Connection handle, or NULL if the listener is closed.
 */
extern xylem_rudp_conn_t* xylem_rudp_accept(xylem_rudp_listener_t* ln);

/**
 * @brief Close a RUDP listener.
 *
 * @note [COROUTINE-ONLY]
 *
 * Resets all active sessions and wakes any parked accept caller without
 * releasing the owner handle. After user operations have returned, call
 * xylem_rudp_destroy_listener().
 *
 * @param ln  Listener handle.
 */
extern void xylem_rudp_close_listener(xylem_rudp_listener_t* ln);

/**
 * @brief Destroy a RUDP listener after user operations have returned.
 *
 * @note [COROUTINE-ONLY]
 *
 * This must be the final call on the listener and must not race with any
 * user operation. Internal dispatcher references may defer the actual free.
 * It closes the listener if needed. Passing NULL is safe.
 *
 * @param ln  Listener handle, or NULL.
 */
extern void xylem_rudp_destroy_listener(xylem_rudp_listener_t* ln);

/**
 * @brief Read data from a stream-mode connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * Suspends until data is available, the connection closes, or
 * the read deadline expires.
 *
 * @param conn  Connection handle.
 * @param buf   Destination buffer.
 * @param len   Buffer size.
 *
 * @return Bytes read (>0), 0 on remote close, -1 on error.
 */
extern int xylem_rudp_read(
    xylem_rudp_conn_t* conn,
    void*              buf,
    int                len);

/**
 * @brief Write data on a stream-mode connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param conn  Connection handle.
 * @param data  Source buffer.
 * @param len   Number of bytes to write.
 *
 * @return 0 on success, -1 on error.
 */
extern int xylem_rudp_write(
    xylem_rudp_conn_t* conn,
    const void*        data,
    int                len);

/**
 * @brief Close a RUDP connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * This function is idempotent and does not release the owner handle. After
 * all user operations have returned, call xylem_rudp_destroy().
 *
 * @param conn  Connection handle.
 */
extern void xylem_rudp_close(xylem_rudp_conn_t* conn);

/**
 * @brief Destroy a RUDP connection after user operations have returned.
 *
 * @note [COROUTINE-ONLY]
 *
 * This must be the final call on the connection and must not race with any
 * user operation. Internal timer or dispatcher references may defer the
 * actual free. It closes the connection if needed. Passing NULL is safe.
 *
 * @param conn  Connection handle, or NULL.
 */
extern void xylem_rudp_destroy(xylem_rudp_conn_t* conn);

/**
 * @brief Set the read deadline for a connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param conn         Connection handle.
 * @param deadline_ms  Absolute xylem_utils_getnow(MSEC) deadline, or 0 to clear.
 */
extern void xylem_rudp_set_read_deadline(
    xylem_rudp_conn_t* conn,
    uint64_t           deadline_ms);

/**
 * @brief Set the write deadline for a connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param conn         Connection handle.
 * @param deadline_ms  Absolute xylem_utils_getnow(MSEC) deadline, or 0 to clear.
 */
extern void xylem_rudp_set_write_deadline(
    xylem_rudp_conn_t* conn,
    uint64_t           deadline_ms);

/**
 * @brief Get the peer address of a connection.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param conn     Connection handle.
 * @param host     Output buffer (at least 46 bytes).
 * @param hostlen  Buffer size.
 * @param port     Output port.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_rudp_remote_addr(
    xylem_rudp_conn_t* conn,
    char*              host,
    int                hostlen,
    uint16_t*          port);

