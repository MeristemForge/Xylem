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

#include "platform/platform-socket.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct iowait_s iowait_t;

/**
 * @brief Result of an iowait_read / iowait_write call.
 *
 * The three values let the caller distinguish the three ways a parked
 * coroutine can be woken up, each of which maps to a different error
 * semantic at the protocol layer.
 */
typedef enum {
    IOWAIT_READY   = 0, /*< fd became readable / writable. */
    IOWAIT_TIMEOUT = 1, /*< timeout_ms elapsed before readiness. */
    IOWAIT_CLOSED  = 2, /*< iowait_close() was invoked. */
} iowait_result_t;

/**
 * @brief Create an IO wait handle bound to a file descriptor.
 *
 * Registers the fd with the shared network poller (netpoll) lazily
 * on the first park call. The fd must already be in non-blocking mode.
 *
 * @param fd    Non-blocking socket descriptor.
 *
 * @return IO wait handle, or NULL on failure.
 */
extern iowait_t* iowait_create(platform_sock_t fd);

/**
 * @brief Suspend the calling coroutine until the fd is readable.
 *
 * Arms the fd on the shared netpoll, then yields. The coroutine is
 * resumed by a scheduler worker when the fd becomes readable, the
 * timeout expires, or the handle is closed.
 *
 * @param w          IO wait handle.
 * @param timeout_ms Timeout in milliseconds, 0 = no timeout.
 *
 * @return IOWAIT_READY on readability, IOWAIT_TIMEOUT on deadline,
 *         IOWAIT_CLOSED if iowait_close() was called.
 */
extern iowait_result_t iowait_read(iowait_t* w, uint64_t timeout_ms);

/**
 * @brief Suspend the calling coroutine until the fd is writable.
 *
 * Arms the fd on the shared netpoll, then yields. The coroutine is
 * resumed by a scheduler worker when the fd becomes writable, the
 * timeout expires, or the handle is closed.
 *
 * @param w          IO wait handle.
 * @param timeout_ms Timeout in milliseconds, 0 = no timeout.
 *
 * @return IOWAIT_READY on writability, IOWAIT_TIMEOUT on deadline,
 *         IOWAIT_CLOSED if iowait_close() was called.
 */
extern iowait_result_t iowait_write(iowait_t* w, uint64_t timeout_ms);

/**
 * @brief Mark the handle as closed and wake all waiting coroutines.
 *
 * After this call, iowait_read/write return IOWAIT_CLOSED immediately.
 * Does NOT close the underlying fd -- the caller owns that.
 *
 * @param w  IO wait handle.
 */
extern void iowait_close(iowait_t* w);

/**
 * @brief Destroy the IO wait handle and release all resources.
 *
 * Removes the fd from the netpoll and frees the handle.
 * The caller must not use the handle after this call.
 *
 * @param w  IO wait handle.
 */
extern void iowait_destroy(iowait_t* w);

/**
 * @brief Check if the handle has been closed.
 *
 * @param w  IO wait handle.
 *
 * @return true if iowait_close() has been called.
 */
extern bool iowait_is_closed(iowait_t* w);

/**
 * @brief Netpoll event callback for iowait handles.
 *
 * Called by the scheduler when a netpoll event fires for an iowait fd.
 * Wakes the appropriate coroutine(s) based on the readiness mask.
 *
 * @param revents  Readiness mask.
 * @param ud       The iowait_t pointer registered as user data.
 */
extern void iowait_on_event(int revents, void* ud);
