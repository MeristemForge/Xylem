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

/**
 * Periodic ticker -- the pull-based counterpart of xylem_timer_every.
 *
 * Unlike a callback timer, the ticker decouples "timekeeping" from
 * "running user code". It is Go-like in the sense that ticks are
 * pull-based and coalesced:
 *
 *   - The internal scheduler timer repeats at the requested interval
 *     after each tiny delivery callback completes. That callback only
 *     does a non-blocking delivery of the tick time, coalescing
 *     (dropping) a tick when the previous one has not been consumed
 *     yet. It never runs user code, so user work can never overlap or
 *     re-enter through the ticker.
 *   - The consumer drains ticks one at a time via xylem_ticker_recv(),
 *     so user logic is naturally serialized.
 *
 * Threading:
 *   - xylem_ticker_recv() is context-adaptive, like the underlying
 *     timer: call it from a coroutine (it parks) or from a plain OS
 *     thread (it blocks the thread). Either way, only one consumer may
 *     receive on a given ticker. This is a caller contract, not
 *     dynamically enforced: concurrent recv calls on the same ticker are
 *     unsupported and may block or steal each other's wakeups. A thread
 *     consumer still requires the runtime to be running, since the ticks
 *     are produced by a scheduler timer.
 *   - xylem_ticker_close() is callable from any thread or context. It
 *     stops future ticks and wakes a consumer already blocked in
 *     xylem_ticker_recv(), which then returns 0.
 *   - xylem_ticker_destroy() consumes the handle and accepts NULL. The
 *     same non-NULL handle must not be destroyed again. Call close first
 *     when another context may be receiving, wait for that receiver to
 *     exit, then destroy the handle. destroy calls close as a cleanup
 *     fallback, but it is not the admission-control boundary for
 *     starting new recv operations.
 *
 * Lifetime:
 *   - The ticker is driven by the runtime scheduler. External OS
 *     threads must not call ticker APIs after xylem_shutdown() has
 *     been called. Stop and join those threads before shutdown, or
 *     make sure they touch no ticker once shutdown begins.
 */
typedef struct xylem_ticker_s xylem_ticker_t;

/**
 * @brief Create and arm a periodic ticker.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * @param interval_ms  Tick period in milliseconds (must be > 0).
 *
 * @return Ticker handle, or NULL on bad input / allocation failure.
 */
extern xylem_ticker_t* xylem_ticker_create(uint64_t interval_ms);

/**
 * @brief Close the ticker and wake a blocked receiver.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * Idempotent. Once closed, xylem_ticker_recv() returns 0. This call
 * does not consume @p ticker; call xylem_ticker_destroy() after the
 * receiver has exited to release the handle.
 *
 * @param ticker  Ticker handle, or NULL (no-op).
 */
extern void xylem_ticker_close(xylem_ticker_t* ticker);

/**
 * @brief Block until the next tick.
 *
 * @note [CONTEXT-ADAPTIVE]
 *
 * Parks the calling coroutine, or blocks the calling OS thread, until
 * the next tick. At most one tick is buffered: if the consumer falls
 * behind, the intervening ticks are dropped (coalesced) rather than
 * queued.
 *
 * @param ticker  Ticker handle, or NULL.
 *
 * @return The tick time in xylem_utils_getnow(MSEC) milliseconds, or 0
 *         when @p ticker is NULL or closed.
 */
extern uint64_t xylem_ticker_recv(xylem_ticker_t* ticker);

/**
 * @brief Close and destroy the ticker.
 *
 * @note [CALLER-SYNCHRONIZED]
 *
 * Calls xylem_ticker_close() as a cleanup fallback, then drops the
 * creator reference. The underlying memory is freed once the last
 * reference is gone. This call consumes the handle and must not race
 * with xylem_ticker_recv(), xylem_ticker_close(), or another destroy on
 * the same handle. Passing NULL is a no-op, but destroying the same
 * non-NULL handle again is invalid.
 * If another context may be in xylem_ticker_recv(), call
 * xylem_ticker_close() first, wait for that receiver to exit, then
 * destroy the handle.
 *
 * @param ticker  Ticker handle, or NULL (no-op).
 */
extern void xylem_ticker_destroy(xylem_ticker_t* ticker);
