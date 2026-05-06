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
 * @brief Result of iowait_read / iowait_write.
 *
 * Distinguishes the three ways a parked coroutine can wake up, each
 * mapping to a different error semantic at the protocol layer.
 */
typedef enum {
    IOWAIT_READY   = 0, /*< fd became readable / writable. */
    IOWAIT_TIMEOUT = 1, /*< deadline reached. */
    IOWAIT_CLOSED  = 2, /*< iowait_close() was invoked. */
} iowait_result_t;

/**
 * @brief Create an IO wait handle bound to a file descriptor.
 *
 * Registers the fd with the shared network poller lazily on the first
 * park call. The fd must already be in non-blocking mode.
 *
 * @param fd  Non-blocking socket descriptor.
 *
 * @return IO wait handle, or NULL on failure.
 */
extern iowait_t* iowait_create(platform_sock_t fd);

/**
 * @brief Set the read deadline, in absolute monotonic milliseconds.
 *
 * Once set, subsequent iowait_read() calls return IOWAIT_TIMEOUT as
 * soon as the clock passes the deadline, even if the call has not
 * yet parked. If iowait_read() is already parked when the deadline
 * passes, it is woken up with IOWAIT_TIMEOUT.
 *
 * The new deadline value is published atomically and is observed by
 * any concurrently parked iowait_read() on the same handle, so this
 * setter may be called while a read is already parked on another
 * thread. However it is NOT safe to invoke concurrently with itself
 * on the same direction: the per-direction timer is allocated lazily
 * on the first non-zero deadline. Intended usage is one logical
 * owner (typically the coroutine that also calls iowait_read) driving
 * the read deadline of a given handle.
 *
 * Passing 0 clears the deadline; any running timer is stopped but
 * may still fire once if its callback was already dispatched.
 *
 * @param w            IO wait handle.
 * @param deadline_ms  Monotonic deadline in ms (see xylem_utils_getnow
 *                     with XYLEM_TIME_PRECISION_MSEC), or 0 to clear.
 */
extern void iowait_set_rd_deadline(iowait_t* w, uint64_t deadline_ms);

/**
 * @brief Set the write deadline, in absolute monotonic milliseconds.
 *
 * Mirror of iowait_set_rd_deadline for the write direction. Same
 * threading constraints apply.
 *
 * @param w            IO wait handle.
 * @param deadline_ms  Monotonic deadline in ms, or 0 to clear.
 */
extern void iowait_set_wr_deadline(iowait_t* w, uint64_t deadline_ms);

/**
 * @brief Suspend the calling coroutine until the fd is readable.
 *
 * Arms the fd on the shared netpoll and yields. The coroutine resumes
 * when the fd becomes readable, the read deadline passes, or
 * iowait_close() is called. Returns immediately with IOWAIT_TIMEOUT
 * if the deadline was already past at entry, or with IOWAIT_CLOSED
 * if the handle is already closed.
 *
 * Only one coroutine may be parked on the read direction of a given
 * handle at a time; concurrent iowait_read() calls on the same handle
 * are not supported.
 *
 * @param w  IO wait handle.
 *
 * @return IOWAIT_READY, IOWAIT_TIMEOUT, or IOWAIT_CLOSED.
 */
extern iowait_result_t iowait_read(iowait_t* w);

/**
 * @brief Suspend the calling coroutine until the fd is writable.
 *
 * Mirror of iowait_read for the write direction. Read and write are
 * independent: a coroutine on read and a coroutine on write may
 * simultaneously park on the same handle.
 *
 * @param w  IO wait handle.
 *
 * @return IOWAIT_READY, IOWAIT_TIMEOUT, or IOWAIT_CLOSED.
 */
extern iowait_result_t iowait_write(iowait_t* w);

/**
 * @brief Mark the handle closed and wake all waiting coroutines.
 *
 * After this call, iowait_read/write return IOWAIT_CLOSED immediately.
 * Idempotent, thread-safe: may be invoked concurrently with park or
 * with a second close; only the first caller actually runs the wake
 * logic. Does NOT close the underlying fd; the caller owns that.
 *
 * @param w  IO wait handle.
 */
extern void iowait_close(iowait_t* w);

/**
 * @brief Destroy the IO wait handle and release all resources.
 *
 * Implicitly closes the handle if not already closed, then drops the
 * creator's reference. The handle is freed once all in-flight
 * references (from active waits and poller callbacks) have been
 * released, so it is safe to call while a waiter is still parked.
 *
 * @param w  IO wait handle (NULL is ignored).
 */
extern void iowait_destroy(iowait_t* w);

/**
 * @brief Check whether the handle has been closed.
 *
 * @param w  IO wait handle.
 *
 * @return true if iowait_close() has been called.
 */
extern bool iowait_is_closed(iowait_t* w);

/**
 * @brief Netpoll event callback.
 *
 * Invoked by the scheduler when a poll event fires for an iowait fd.
 * Wakes the appropriate coroutine(s) based on the readiness mask, and
 * under level-triggered+oneshot pollers re-arms the fd if anyone is
 * still parked. Under edge-triggered pollers the fd stays armed after
 * its initial registration.
 *
 * @param revents  Readiness mask.
 * @param ud       The iowait_t pointer registered as user data.
 */
extern void iowait_on_event(int revents, void* ud);
