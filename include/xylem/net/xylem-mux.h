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

#include "xylem/sync/xylem-channel.h"
#include "xylem/sync/xylem-mutex.h"

#include <stddef.h>
#include <stdint.h>

typedef struct xylem_mux_s        xylem_mux_t;
typedef struct xylem_mux_stream_s xylem_mux_stream_t;

typedef int64_t (*xylem_mux_read_fn)(void* ctx, void* buf, size_t len);
typedef int (*xylem_mux_write_fn)(void* ctx, const void* data, size_t len);

typedef enum xylem_mux_role_e {
    XYLEM_MUX_CLIENT, /*< Open streams use odd IDs (1,3,5...). */
    XYLEM_MUX_SERVER  /*< Open streams use even IDs (2,4,6...). */
} xylem_mux_role_t;

typedef struct xylem_mux_opts_s {
    uint32_t max_stream_window; /*< Per-stream flow control window, 0 = 256KB. */
    uint32_t max_streams;       /*< Max concurrent streams, 0 = unlimited. */
    uint64_t keepalive_ms;      /*< Ping interval, 0 = disabled. */
} xylem_mux_opts_t;

/**
 * @brief Create a mux session over a reliable byte stream.
 *
 * Spawns a background reader coroutine that demultiplexes incoming
 * frames and dispatches them to the appropriate streams.
 *
 * @param ctx    Opaque transport context passed to read/write.
 * @param read   Transport read callback (same signature as tcp/tls recv).
 * @param write  Transport write callback (same signature as tcp/tls send).
 * @param role   Client or server (determines stream ID allocation).
 * @param opts   Options, or NULL for defaults.
 *
 * @return Session handle, or NULL on failure.
 */
extern xylem_mux_t* xylem_mux_create(
    void*              ctx,
    xylem_mux_read_fn  read,
    xylem_mux_write_fn write,
    xylem_mux_role_t   role,
    xylem_mux_opts_t*  opts);

/**
 * @brief Close the mux session.
 *
 * Sends GoAway, resets all streams, and releases resources.
 *
 * @param mux  Session handle.
 */
extern void xylem_mux_close(xylem_mux_t* mux);

/**
 * @brief Open a new stream on the session.
 *
 * Sends a SYN frame to the peer. The stream is immediately usable.
 *
 * @param mux  Session handle.
 *
 * @return Stream handle, or NULL if the session is closed.
 */
extern xylem_mux_stream_t* xylem_mux_open_stream(xylem_mux_t* mux);

/**
 * @brief Accept an incoming stream opened by the peer.
 *
 * Suspends the calling coroutine until a new stream arrives or
 * the session closes.
 *
 * @param mux  Session handle.
 *
 * @return Stream handle, or NULL if the session is closed.
 */
extern xylem_mux_stream_t* xylem_mux_accept_stream(xylem_mux_t* mux);

/**
 * @brief Receive data from a stream.
 *
 * Suspends until data is available, the remote end closes, or
 * the deadline expires.
 *
 * @param s    Stream handle.
 * @param buf  Destination buffer.
 * @param len  Buffer size.
 *
 * @return Bytes read (>0), 0 on remote close, -1 on error/reset.
 */
extern int64_t xylem_mux_recv(
    xylem_mux_stream_t* s, void* buf, size_t len);

/**
 * @brief Send data on a stream.
 *
 * Suspends if the peer's receive window is full. All bytes are
 * written before returning.
 *
 * @param s     Stream handle.
 * @param data  Source buffer.
 * @param len   Number of bytes to send.
 *
 * @return 0 on success, -1 on error/reset.
 */
extern int xylem_mux_send(
    xylem_mux_stream_t* s, const void* data, size_t len);

/**
 * @brief Close a stream. Sends FIN to the peer.
 *
 * @param s  Stream handle.
 */
extern void xylem_mux_stream_close(xylem_mux_stream_t* s);

/**
 * @brief Set the read deadline for a stream.
 *
 * @param s            Stream handle.
 * @param deadline_ms  Monotonic deadline in ms, or 0 to clear.
 */
extern void xylem_mux_set_read_deadline(
    xylem_mux_stream_t* s, uint64_t deadline_ms);

/**
 * @brief Set the write deadline for a stream.
 *
 * @param s            Stream handle.
 * @param deadline_ms  Monotonic deadline in ms, or 0 to clear.
 */
extern void xylem_mux_set_write_deadline(
    xylem_mux_stream_t* s, uint64_t deadline_ms);
