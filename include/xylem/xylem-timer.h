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
 * timer APIs after xylem_shutdown() has been called.
 */

/**
 * @brief Arm a one-shot timer.
 *
 * @note [THREAD-SAFE]
 *
 * The handle must be consumed by xylem_timer_cancel(), even after the
 * callback has fired. Timer APIs must not be called from external OS
 * threads after xylem_shutdown() has been called.
 *
 * @param delay_ms  Delay in milliseconds.
 * @param cb        Callback to invoke on expiry.
 * @param ud        Opaque user data passed to @p cb.
 *
 * @return Timer handle, or NULL on allocation failure.
 */
extern xylem_timer_t* xylem_timer_after(
    uint64_t delay_ms, xylem_timer_fn_t cb, void* ud);

/**
 * @brief Arm a periodic timer.
 *
 * @note [THREAD-SAFE]
 *
 * The first fire occurs after interval_ms. Subsequent fires are scheduled
 * only after the previous callback returns, so callbacks for the same timer
 * never overlap. The handle must be consumed by xylem_timer_cancel().
 *
 * @param interval_ms  Delay between callback completions, in milliseconds.
 * @param cb           Callback to invoke on each expiry.
 * @param ud           Opaque user data passed to @p cb.
 *
 * @return Timer handle, or NULL on allocation failure or interval_ms == 0.
 */
extern xylem_timer_t* xylem_timer_every(
    uint64_t interval_ms, xylem_timer_fn_t cb, void* ud);

/**
 * @brief Cancel the timer and release the handle.
 *
 * @note [THREAD-SAFE]
 *
 * A callback already in flight may still run to completion. This call
 * consumes @p timer; the handle must not be used again after it returns.
 * Calls on different timer handles may run concurrently, but operations
 * on the same handle, including cancel/cancel and cancel/reset, require
 * external synchronization.
 * Safe with @p timer == NULL (no-op, returns false).
 *
 * @param timer  Timer handle, or NULL.
 *
 * @return true if a pending fire was cancelled before it ran.
 */
extern bool xylem_timer_cancel(xylem_timer_t* timer);

/**
 * @brief Re-arm a timer with a new delay.
 *
 * @note [THREAD-SAFE]
 *
 * Preserves callback and user data. Restarts the countdown from now.
 * For periodic timers, delay_ms also becomes the new interval.
 * Calls on different timer handles may run concurrently, but operations
 * on the same handle, including reset/reset and reset/cancel, require
 * external synchronization. If both cancel and reset are issued during
 * the same callback, cancel takes priority and the reset is silently
 * dropped.
 * Safe with @p timer == NULL (no-op, returns false).
 *
 * @param timer     Timer handle, or NULL.
 * @param delay_ms  Delay in milliseconds until the next fire.
 *
 * @return true if a pending fire was cancelled before it ran.
 */
extern bool xylem_timer_reset(xylem_timer_t* timer, uint64_t delay_ms);
