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
 * iowait threading model
 * ----------------------
 *
 * An iowait handle is one-reader / one-writer per direction: at most
 * one coroutine may be parked on `iowait_read` and at most one on
 * `iowait_write` at the same time. Read and write are independent;
 * they may be parked by two different coroutines simultaneously.
 *
 * Wake sources (an IO event, a deadline timer, iowait_close) race
 * through a single arbitrator per direction, so the parked coroutine
 * is woken exactly once with the winning cause stamped into the
 * iowait_result_t return value.
 *
 * Deadline setters, iowait_close, iowait_destroy, and iowait_is_closed
 * are all safe to call from any thread, including while a read or
 * write is parked on another thread. The one exception is the
 * deadline setter on a single direction: concurrent setters on the
 * same direction of the same handle are not safe (the per-direction
 * deadline timer is allocated lazily). Normal usage is for one
 * logical owner (typically the same coroutine that also reads or
 * writes) to drive the deadline.
 */
/**
 * @brief Result of iowait_read / iowait_write.
 *
 * Distinguishes the three ways a parked coroutine can wake up, each
 * mapping to a different error semantic at the protocol layer.
 */
typedef enum iowait_result_e {
    IOWAIT_READY   = 0, /*< fd became readable / writable. */
    IOWAIT_TIMEOUT = 1, /*< deadline reached. */
    IOWAIT_CLOSED  = 2, /*< iowait_close() was invoked. */
} iowait_result_t;

/**
 * @brief Create an IO wait handle bound to a file descriptor.
 *
 * The handle defers poller registration to the first park call; the
 * exact registration pattern (one-shot re-arm vs. edge-triggered one-
 * time add) is internal and depends on the platform poller. The fd
 * must already be in non-blocking mode, and must not be shared with
 * another iowait concurrently.
 *
 * @param fd  Non-blocking socket descriptor.
 *
 * @return IO wait handle, or NULL on failure.
 */
extern iowait_t* iowait_create(platform_sock_t fd);

/**
 * @brief Set the read deadline, in absolute monotonic milliseconds.
 *
 * Once set, subsequent or in-flight iowait_read() calls on the same
 * handle return IOWAIT_TIMEOUT as soon as the clock passes the
 * deadline. Passing 0 clears the deadline; any running timer is
 * stopped but may still fire once if its callback was already
 * dispatched at the moment of the stop.
 *
 * See the threading-model comment at the top of this header for the
 * concurrency rules (one logical owner per direction).
 *
 * @param w            IO wait handle.
 * @param deadline_ms  Monotonic deadline in ms (see xylem_utils_getnow
 *                     with XYLEM_TIME_PRECISION_MSEC), or 0 to clear.
 */
extern void iowait_set_rd_deadline(iowait_t* w, uint64_t deadline_ms);

/**
 * @brief Set the write deadline, in absolute monotonic milliseconds.
 *
 * Mirror of iowait_set_rd_deadline for the write direction.
 *
 * @param w            IO wait handle.
 * @param deadline_ms  Monotonic deadline in ms, or 0 to clear.
 */
extern void iowait_set_wr_deadline(iowait_t* w, uint64_t deadline_ms);

/**
 * @brief Suspend the calling coroutine until the fd is readable.
 *
 * Arms the fd on the scheduler's poller and yields. The coroutine
 * resumes when the fd becomes readable, the read deadline passes,
 * or iowait_close() is called. Returns immediately with
 * IOWAIT_TIMEOUT if the deadline was already past at entry, or with
 * IOWAIT_CLOSED if the handle was already closed. Must be called
 * from inside a coroutine running on the scheduler.
 *
 * See the threading-model comment at the top of this header for
 * the one-reader-per-direction rule.
 *
 * @param w  IO wait handle.
 *
 * @return IOWAIT_READY, IOWAIT_TIMEOUT, or IOWAIT_CLOSED.
 */
extern iowait_result_t iowait_read(iowait_t* w);

/**
 * @brief Suspend the calling coroutine until the fd is writable.
 *
 * Mirror of iowait_read for the write direction.
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
 * Drops the kernel poller subscription synchronously, so the caller
 * can safely close the underlying fd right after without racing a
 * deferred EPOLL_CTL_DEL against a recycled fd number.
 *
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
 * Decodes the generation-tagged ud that was registered, rejects
 * stale CQEs whose target handle has been recycled since
 * registration, and otherwise wakes the parked coroutine(s)
 * according to the readiness mask. Under LT+oneshot pollers, re-
 * arms the fd if anyone is still parked; under ET the fd stays
 * armed after its initial registration.
 *
 * @param revents  Readiness mask.
 * @param ud       Opaque userdata that was registered via the
 *                 platform poller at iowait creation (internally a
 *                 generation-tagged iowait pointer).
 */
extern void iowait_on_event(int revents, void* ud);

/**
 * @brief Opaque per-scheduler iowait handle pool.
 *
 * The pool recycles iowait handles so their backing memory stays
 * type-stable across their lifetime. This lets iowait_on_event safely
 * dispatch against a possibly-retired handle by verifying a
 * generation counter before touching request-specific state. Every
 * scheduler owns exactly one pool; iowait_create() looks it up via
 * the runtime's current scheduler.
 */
typedef struct iowait_pool_s iowait_pool_t;

/**
 * @brief Create an iowait handle pool.
 *
 * @return New pool, or NULL on failure.
 */
extern iowait_pool_t* iowait_pool_create(void);

/**
 * @brief Destroy an iowait handle pool and free every handle it has
 * ever minted.
 *
 * Must only be called after the scheduler has been destroyed -- at
 * that point no worker threads are alive, so there are no in-flight
 * CQEs that could dereference pooled memory.
 *
 * @param pool  Pool to destroy, or NULL (no-op).
 */
extern void iowait_pool_destroy(iowait_pool_t* pool);
