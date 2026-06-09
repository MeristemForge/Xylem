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
 * Periodic ticker -- the repeating counterpart of xylem_timer_after.
 *
 * Unlike a one-shot timer, the ticker decouples "timekeeping" from
 * "running user code", exactly like Go's time.Ticker:
 *
 *   - The internal timer fires on a fixed period and only does a
 *     non-blocking delivery of the tick time, coalescing (dropping)
 *     a tick when the previous one has not been consumed yet. It
 *     never runs user code, so ticks can never overlap or re-enter.
 *   - The consumer drains ticks one at a time via xylem_ticker_recv(),
 *     so user logic is naturally serialized.
 *
 * Threading:
 *   - xylem_ticker_recv() is context-adaptive, like the underlying
 *     timer: call it from a coroutine (it parks) or from a plain OS
 *     thread (it blocks the thread). Either way, only one consumer may
 *     receive on a given ticker (single-consumer). A thread consumer
 *     still requires the runtime to be running, since the ticks are
 *     produced by a scheduler timer.
 *   - xylem_ticker_destroy() is callable from any thread or context;
 *     it is idempotent and safe to call once from the consumer side.
 */
typedef struct xylem_ticker_s xylem_ticker_t;

/**
 * @brief Create and arm a periodic ticker. Thread-safe.
 *
 * @param interval_ms  Tick period in milliseconds (must be > 0).
 *
 * @return Ticker handle, or NULL on bad input / allocation failure.
 */
extern xylem_ticker_t* xylem_ticker_create(uint64_t interval_ms);

/**
 * @brief Block until the next tick.
 *
 * Context-adaptive.
 *
 * Parks the calling coroutine, or blocks the calling OS thread, until
 * the next tick. At most one tick is buffered: if the consumer falls
 * behind, the intervening ticks are dropped (coalesced) rather than
 * queued.
 *
 * @param ticker  Ticker handle.
 *
 * @return The tick time in milliseconds (monotonic), or 0 once the
 *         ticker has been stopped and drained.
 */
extern uint64_t xylem_ticker_recv(xylem_ticker_t* ticker);

/**
 * @brief Stop ticking and destroy the ticker. Thread-safe, idempotent.
 *
 * Wakes a consumer blocked in xylem_ticker_recv() (which then returns
 * 0). The underlying memory is freed once the last reference is gone,
 * so it is safe to call even while a recv is in flight.
 *
 * @param ticker  Ticker handle, or NULL (no-op).
 */
extern void xylem_ticker_destroy(xylem_ticker_t* ticker);
