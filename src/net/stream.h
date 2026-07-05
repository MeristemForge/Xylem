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

#include "net/addr.h"
#include "platform/platform-socket.h"
#include "runtime/iowait.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct stream_s   stream_t;
typedef struct listener_s listener_t;

#define STREAM_IO_AGAIN (-2)

/**
 * @brief Wrap an already connected fd in the coroutine stream core.
 *
 * The stream takes ownership of fd on success.
 *
 * @param fd  Connected socket.
 *
 * @return Stream handle, or NULL on allocation failure.
 */
extern stream_t* stream_from_fd(platform_sock_t fd);

/**
 * @brief Wrap a listening fd in the coroutine listener core.
 *
 * The listener takes ownership of fd on success.
 *
 * @param fd  Listening socket.
 *
 * @return Listener handle, or NULL on allocation failure.
 */
extern listener_t* listener_from_fd(platform_sock_t fd);

/**
 * @brief Dial a peer and return a coroutine stream.
 *
 * @param host                Remote hostname or address.
 * @param port                Remote port.
 * @param connect_timeout_ms  Connect timeout in ms, 0 for none.
 * @param enable_mss_clamp    Whether to clamp MSS on the socket.
 *
 * @return Stream handle, or NULL on failure.
 */
extern stream_t* stream_dial(
    const char* host,
    uint16_t    port,
    uint64_t    connect_timeout_ms,
    bool        enable_mss_clamp);

/**
 * @brief Interrupt blocked stream operations without releasing ownership.
 *
 * @param stream  Stream handle.
 */
extern void stream_interrupt(stream_t* stream);

/**
 * @brief Release one stream ownership reference.
 *
 * @param stream  Stream handle.
 */
extern void stream_release(stream_t* stream);

/**
 * @brief Set the stream read deadline.
 *
 * @param stream       Stream handle.
 * @param deadline_ms  Absolute monotonic timestamp in ms, or 0 to clear.
 */
extern void stream_set_read_deadline(
    stream_t* stream,
    uint64_t  deadline_ms);

/**
 * @brief Set the stream write deadline.
 *
 * @param stream       Stream handle.
 * @param deadline_ms  Absolute monotonic timestamp in ms, or 0 to clear.
 */
extern void stream_set_write_deadline(
    stream_t* stream,
    uint64_t  deadline_ms);

/**
 * @brief Read available bytes from the stream.
 *
 * @param stream  Stream handle.
 * @param buf     Destination buffer.
 * @param len     Maximum bytes to read.
 *
 * @return Bytes read, 0 on peer close, or -1 on error/timeout.
 */
extern int stream_read(stream_t* stream, void* buf, int len);

/**
 * @brief Try one non-blocking stream read.
 *
 * @param stream  Stream handle.
 * @param buf     Destination buffer.
 * @param len     Maximum bytes to read.
 *
 * @return Bytes read, 0 on peer close, STREAM_IO_AGAIN on EAGAIN, or -1 on
 * error/timeout.
 */
extern int stream_try_read(
    stream_t* stream,
    void*     buf,
    int       len);

/**
 * @brief Wait for read readiness.
 *
 * @param stream  Stream handle.
 *
 * @return Readiness result.
 */
extern iowait_result_t stream_wait_read(stream_t* stream);

/**
 * @brief Write the full buffer to the stream.
 *
 * @param stream  Stream handle.
 * @param data    Source buffer.
 * @param len     Number of bytes to write.
 *
 * @return 0 on success, -1 on error/timeout.
 */
extern int stream_write(
    stream_t* stream,
    const void* data,
    int         len);

/**
 * @brief Try one non-blocking stream write.
 *
 * @param stream  Stream handle.
 * @param data    Source buffer.
 * @param len     Maximum bytes to write.
 *
 * @return Bytes written (>0), 0 when len is 0, STREAM_IO_AGAIN on EAGAIN,
 * or -1 on error/timeout.
 */
extern int stream_try_write(
    stream_t*   stream,
    const void* data,
    int         len);

/**
 * @brief Wait for write readiness.
 *
 * @param stream  Stream handle.
 *
 * @return Readiness result.
 */
extern iowait_result_t stream_wait_write(stream_t* stream);

/**
 * @brief Get the stream's remote address.
 *
 * @param stream    Stream handle.
 * @param host      Output host buffer.
 * @param host_len  Output host buffer length.
 * @param port      Output port pointer.
 *
 * @return 0 on success, -1 on failure.
 */
extern int stream_remote_addr(
    stream_t* stream,
    char*     host,
    size_t    host_len,
    uint16_t* port);

/**
 * @brief Get the stream's local address.
 *
 * @param stream    Stream handle.
 * @param host      Output host buffer.
 * @param host_len  Output host buffer length.
 * @param port      Output port pointer.
 *
 * @return 0 on success, -1 on failure.
 */
extern int stream_local_addr(
    stream_t* stream,
    char*     host,
    size_t    host_len,
    uint16_t* port);

/**
 * @brief Shut down the stream write direction.
 *
 * @param stream  Stream handle.
 *
 * @return 0 on success, -1 on failure.
 */
extern int stream_shutdown_wr(stream_t* stream);

/**
 * @brief Shut down the stream read direction.
 *
 * @param stream  Stream handle.
 *
 * @return 0 on success, -1 on failure.
 */
extern int stream_shutdown_rd(stream_t* stream);

/**
 * @brief Return the platform socket owned by the stream.
 *
 * @param stream  Stream handle.
 *
 * @return Platform socket handle.
 */
extern platform_sock_t stream_fd(stream_t* stream);

/**
 * @brief Create a listener.
 *
 * @param host              Bind address, or NULL for any.
 * @param port              Bind port.
 * @param enable_mss_clamp  Whether to clamp MSS on accepted sockets.
 *
 * @return Listener handle, or NULL on failure.
 */
extern listener_t* listener_listen(
    const char* host,
    uint16_t    port,
    bool        enable_mss_clamp);

/**
 * @brief Accept a stream from a listener.
 *
 * @param listener  Listener handle.
 *
 * @return Accepted stream, or NULL if the listener is closed.
 */
extern stream_t* listener_accept(listener_t* listener);

/**
 * @brief Interrupt blocked listener operations without releasing ownership.
 *
 * @param listener  Listener handle.
 */
extern void listener_interrupt(listener_t* listener);

/**
 * @brief Release one listener ownership reference.
 *
 * @param listener  Listener handle.
 */
extern void listener_release(listener_t* listener);

/**
 * @brief Get the listener's local address.
 *
 * @param listener  Listener handle.
 * @param host      Output host buffer.
 * @param host_len  Output host buffer length.
 * @param port      Output port pointer.
 *
 * @return 0 on success, -1 on failure.
 */
extern int listener_addr(
    listener_t* listener,
    char*       host,
    size_t      host_len,
    uint16_t*   port);
