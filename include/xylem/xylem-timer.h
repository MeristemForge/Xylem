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
#include <stdint.h>

typedef struct xylem_timer_s xylem_timer_t;

/* Timer expiry callback: t is the timer that fired, ud the user data. */
typedef void (*xylem_timer_fn_t)(xylem_timer_t* t, void* ud);

/**
 * One-shot and periodic scheduler timers.
 *
 * Lifetime: timers are driven by the runtime scheduler; all timer APIs
 * require xylem_run() to be active. External OS threads must not call
 * timer APIs after xylem_shutdown() has been called. A timer handle is a
 * single-owner resource: calls on the same handle must be serialized by the
 * caller. xylem_timer_stop() and xylem_timer_reset() do not consume the
 * handle and may be called from its callback. xylem_timer_destroy() consumes
 * the handle and must not be called from its callback.
 *
 * Timer callbacks run in newly spawned coroutines. Dispatch is best-effort
 * under allocation failure: if the runtime cannot allocate the per-fire
 * context or callback coroutine, that fire is dropped without an asynchronous
 * error. A one-shot fire is not retried; a periodic timer continues with its
 * next interval.
 */

/**
 * @brief Arm a one-shot timer.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * The returned handle must be consumed exactly once by
 * xylem_timer_destroy(), even after the callback has fired. Timer APIs must
 * not be called from external OS threads after xylem_shutdown() has been
 * called.
 *
 * @param delay_ms  Delay in milliseconds (must be > 0).
 * @param cb        Callback to invoke on expiry.
 * @param ud        Opaque user data passed to @p cb.
 *
 * @return Timer handle, or NULL on bad input / allocation failure.
 */
extern xylem_timer_t* xylem_timer_after(
    uint64_t delay_ms, xylem_timer_fn_t cb, void* ud);

/**
 * @brief Arm a periodic timer.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * The first fire occurs after interval_ms. Subsequent fires are scheduled
 * only after the previous callback returns, so callbacks for the same timer
 * never overlap. The returned handle must be consumed exactly once by
 * xylem_timer_destroy().
 *
 * @param interval_ms  Delay between callback completions, in milliseconds.
 * @param cb           Callback to invoke on each expiry.
 * @param ud           Opaque user data passed to @p cb.
 *
 * @return Timer handle, or NULL on bad input / allocation failure.
 */
extern xylem_timer_t* xylem_timer_every(
    uint64_t interval_ms, xylem_timer_fn_t cb, void* ud);

/**
 * @brief Stop the timer without releasing its handle.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * A callback already in flight may still run to completion. This function
 * may be called from the timer callback. The stopped timer may be re-armed
 * with xylem_timer_reset(). If stop and reset are both called during the same
 * callback, stop takes priority and the reset is dropped. Safe with
 * @p timer == NULL.
 *
 * @param timer  Timer handle, or NULL.
 *
 * @return true if a queued fire was cancelled before it ran, or if a
 *         deferred reset from an in-flight callback was cancelled.
 */
extern bool xylem_timer_stop(xylem_timer_t* timer);

/**
 * @brief Re-arm a timer with a new delay.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * Preserves callback and user data. Restarts the countdown from now.
 * For periodic timers, delay_ms also becomes the new interval and must be
 * greater than 0. @p timer must be a live handle that has not been destroyed.
 * Calls on different timer handles may run concurrently, but operations
 * on the same handle, including reset/reset and reset/stop, require external
 * synchronization. If both stop and reset are issued during the same
 * callback, stop takes priority and the reset is silently dropped.
 * Safe with @p timer == NULL (no-op, returns false).
 *
 * @param timer     Timer handle, or NULL.
 * @param delay_ms  Delay in milliseconds until the next fire (must be > 0).
 *
 * @return true if reset cancelled a queued fire before it ran, or if it
 *         overwrote an earlier deferred reset from the same in-flight
 *         callback. false may still mean the timer was re-armed when no
 *         pending or deferred fire was cancelled.
 */
extern bool xylem_timer_reset(xylem_timer_t* timer, uint64_t delay_ms);

/**
 * @brief Stop the timer and release its handle.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * Stops the timer as a cleanup fallback, then drops the owner's reference.
 * This must be the final externally synchronized call on @p timer. It must
 * not be called from the timer callback or concurrently with another
 * operation on the same handle. This function does not wait for an already
 * dispatched callback to return; the caller must keep callback and user-data
 * resources alive until that callback completes. Passing NULL is a no-op.
 *
 * @param timer  Timer handle, or NULL.
 */
extern void xylem_timer_destroy(xylem_timer_t* timer);
