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

/**
 * @brief Timer expiry callback.
 *
 * Not invoked in a coroutine context; do not call coroutine-only
 * primitives (xylem_sleep and friends) from here. For blocking or
 * long-running work, xylem_spawn() a coroutine from the callback.
 *
 * @param t   The timer that fired.
 * @param ud  User data from xylem_timer_after() / xylem_timer_every().
 */
typedef void (*xylem_timer_fn_t)(xylem_timer_t* t, void* ud);

/**
 * @brief Arm a one-shot timer. Thread-safe.
 *
 * Invokes @p cb(t, ud) once, @p delay_ms milliseconds from now. Must
 * be called while a xylem runtime is running.
 *
 * The handle must be released with xylem_timer_cancel(), even after
 * the callback has fired. The callback does not auto-release.
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
 * @brief Arm a periodic timer. Thread-safe.
 *
 * Invokes @p cb(t, ud) every @p period_ms milliseconds, starting
 * @p period_ms from now. Fires do not overlap: if a previous cb is
 * still running when the next period elapses, the next fire is not
 * dispatched in parallel.
 *
 * @param period_ms  Period in milliseconds. Must be non-zero.
 * @param cb         Callback to invoke on each fire.
 * @param ud         Opaque user data passed to @p cb.
 *
 * @return Timer handle, or NULL on allocation failure.
 */
extern xylem_timer_t* xylem_timer_every(
    uint64_t period_ms, xylem_timer_fn_t cb, void* ud);

/**
 * @brief Cancel the timer and release the handle. Thread-safe.
 *
 * After this call returns the handle must not be used and the
 * callback will not be invoked again. A callback that was already
 * dispatched but has not yet returned may still run to completion
 * concurrently with this call; the handle stays alive for that
 * callback.
 *
 * Safe with @p t == NULL (no-op, returns false).
 *
 * `ud`'s backing storage lifetime is the caller's responsibility.
 * Typical pattern: free it right after xylem_timer_cancel() returns
 * true (you caught the pending fire), or from the last callback
 * invocation that you know will happen before cancelling.
 *
 * @param t  Timer handle, or NULL.
 *
 * @return true if a pending fire was cancelled before it ran.
 */
extern bool xylem_timer_cancel(xylem_timer_t* t);

/**
 * @brief Re-arm a timer with a new delay. Thread-safe.
 *
 * Preserves the previously configured callback and user data, and
 * restarts the timer's clock from now:
 *   - one-shot timers (xylem_timer_after) fire once, @p delay_ms
 *     from now. Resetting is supported both before and after the
 *     previous one-shot fire has been dispatched; the latter case
 *     re-arms the handle for a fresh single countdown.
 *   - periodic timers (xylem_timer_every) fire next in @p delay_ms,
 *     and @p delay_ms becomes the new repeat interval for every
 *     subsequent fire.
 *
 * Typical uses: sliding idle-timeout ("reset the deadline whenever
 * data arrives"), or adjusting a periodic timer's cadence without
 * re-creating it.
 *
 * Safe with @p t == NULL (no-op, returns false).
 *
 * @param t         Timer handle, or NULL.
 * @param delay_ms  New delay in milliseconds. Also becomes the new
 *                  period for periodic timers.
 *
 * @return true if a pending fire was cancelled before it ran.
 */
extern bool xylem_timer_reset(xylem_timer_t* t, uint64_t delay_ms);
