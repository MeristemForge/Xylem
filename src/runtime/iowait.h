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

#include "scheduler.h"

#include "platform/platform-socket.h"

#include <stdbool.h>
#include <stdint.h>

/* Opaque I/O wait handle (per-fd, per-direction park). */
typedef struct iowait_s iowait_t;

/**
 * iowait concurrency model
 *
 * An iowait handle is one-reader / one-writer per direction: at most
 * one coroutine may be parked on `iowait_read` and at most one on
 * `iowait_write` at the same time. Read and write are independent;
 * they may be parked by two different coroutines simultaneously.
 * Violating this rule is detected when the second parker tries to
 * publish its park record and aborts the process with a diagnostic
 * log; it is not silently tolerated.
 *
 * Wake sources (an IO event, a deadline timer, iowait_close) race
 * through a single arbitrator per direction, so the parked coroutine
 * is resumed at most once per wait attempt. The iowait_result_t return
 * value is then derived from the state re-checked on resume in this
 * order: CLOSED, ERROR, TIMEOUT, READY. In particular, a readiness wake
 * that arrives first may still return IOWAIT_TIMEOUT if the coroutine
 * resumes after the deadline has passed.
 *
 * Deadline setters and iowait_close are safe to call from any thread,
 * including while a read or write is parked on another thread.
 * Concurrent setters on the same direction are serialized internally.
 * iowait_destroy is the final owner-side release, not a synchronization
 * primitive: it must not race with any other iowait API call, and all
 * parked waiters must have returned before it is called.
 */

/**
 * Result of iowait_read / iowait_write.
 *
 * Distinguishes the ways a parked coroutine can wake up, each
 * mapping to a different error semantic at the protocol layer.
 */
typedef enum iowait_result_e {
    IOWAIT_READY   = 0, /* fd became readable / writable. */
    IOWAIT_TIMEOUT = 1, /* deadline reached. */
    IOWAIT_CLOSED  = 2, /* iowait_close() was invoked. */
    IOWAIT_ERROR   = 3, /* Deadline timer arm failed. */
} iowait_result_t;

/**
 * @brief Create an IO wait handle bound to a file descriptor.
 *
 * Registers the fd for read+write readiness on the scheduler's poller
 * immediately. On ET pollers the registration is persistent; on LT+ONESHOT
 * pollers iowait_on_event re-registers after each event. The fd must
 * already be in non-blocking mode, and must not be shared with another
 * iowait concurrently.
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
 * dispatched at the moment of the stop. If that wake reaches a parked
 * coroutine after the deadline has been cleared or reset, the wake is
 * treated as spurious and the wait retries instead of returning
 * IOWAIT_TIMEOUT.
 *
 * Safe to call from any thread, including while a read is parked on
 * another thread. See the "iowait concurrency model" comment at the
 * top of this header for the full rules.
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
 * @brief Check whether the read deadline has expired.
 *
 * This is for non-blocking transport fast paths that attempt a socket
 * operation before parking in iowait_read().
 *
 * @param w  IO wait handle.
 *
 * @return true when the read deadline is set and already expired.
 */
extern bool iowait_read_deadline_expired(iowait_t* w);

/**
 * @brief Check whether the write deadline has expired.
 *
 * Mirror of iowait_read_deadline_expired for write fast paths.
 *
 * @param w  IO wait handle.
 *
 * @return true when the write deadline is set and already expired.
 */
extern bool iowait_write_deadline_expired(iowait_t* w);

/**
 * @brief Suspend the calling coroutine until the fd is readable.
 *
 * Yields the calling coroutine, which resumes when the fd becomes
 * readable, the read deadline passes, or iowait_close() is called.
 * Returns immediately with IOWAIT_TIMEOUT if the deadline was already
 * past at entry, or with IOWAIT_CLOSED if the handle was already
 * closed. Must be called from inside a coroutine running on the
 * scheduler.
 *
 * See the concurrency-model comment at the top of this header for
 * the one-reader-per-direction rule.
 *
 * @param w  IO wait handle.
 *
 * @return IOWAIT_READY, IOWAIT_TIMEOUT, IOWAIT_CLOSED, or IOWAIT_ERROR.
 */
extern iowait_result_t iowait_read(iowait_t* w);

/**
 * @brief Suspend the calling coroutine until the fd is writable.
 *
 * Mirror of iowait_read for the write direction.
 *
 * @param w  IO wait handle.
 *
 * @return IOWAIT_READY, IOWAIT_TIMEOUT, IOWAIT_CLOSED, or IOWAIT_ERROR.
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
 * references (held by in-flight poller callbacks and armed deadline
 * timers, not by parked waiters) have been released. This is the final
 * owner-side release and must not race with any other iowait API call.
 * A parked waiter does not hold a reference, so the caller must keep the
 * handle alive until every reader and writer has returned -- typically
 * by reference-counting the owning connection and destroying the iowait
 * only after the owner reaches its final release.
 *
 * @param w  IO wait handle (NULL is ignored).
 */
extern void iowait_destroy(iowait_t* w);

/**
 * @brief Process a single I/O completion event.
 *
 * Rejects stale CQEs via generation tag, wakes parked coroutines
 * into @p batch. Caller flushes the batch after the poll pass.
 *
 * @param sched    Scheduler handle.
 * @param revents  Readiness mask.
 * @param ud       Generation-tagged slab index from the poller.
 * @param batch    Accumulator for ready coroutines. Must not be NULL.
 */
extern void iowait_on_event(
    scheduler_t*      sched,
    int               revents,
    void*             ud,
    runnable_batch_t* batch);

/**
 * Opaque per-scheduler iowait handle slab.
 *
 * Paged slab allocator that keeps retired iowait handles alive in
 * type-stable memory. Poller ud carries a (gen, slab-index) pair
 * packed into a uintptr_t, supporting both 32-bit and 64-bit
 * platforms. Every scheduler owns exactly one slab; iowait_create()
 * looks it up via the runtime's current scheduler.
 */
typedef struct iowait_slab_s iowait_slab_t;

/**
 * @brief Create an iowait handle slab.
 *
 * @return New slab, or NULL on failure.
 */
extern iowait_slab_t* iowait_slab_create(void);

/**
 * @brief Destroy an iowait handle slab and free every page.
 *
 * Must only be called after the scheduler has been destroyed -- at
 * that point no worker threads are alive, so there are no in-flight
 * CQEs that could dereference slab memory.
 *
 * @param slab  Slab to destroy, or NULL (no-op).
 */
extern void iowait_slab_destroy(iowait_slab_t* slab);

