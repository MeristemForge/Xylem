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

#include <stdint.h>

typedef struct sched_timer_s     sched_timer_t;
typedef struct sched_timer_mgr_s sched_timer_mgr_t;

/**
 * @brief Timer expiry callback.
 *
 * Called on a worker thread when the timer fires.
 *
 * @param timer  The timer that fired.
 * @param ud     User data from sched_timer_start.
 */
typedef void (*sched_timer_fn_t)(sched_timer_t* timer, void* ud);

/**
 * @brief Create a timer manager.
 *
 * @return Manager handle, or NULL on failure.
 */
extern sched_timer_mgr_t* sched_timer_mgr_create(void);

/**
 * @brief Destroy a timer manager.
 *
 * All timers must be stopped/destroyed before calling this.
 *
 * @param mgr  Manager handle.
 */
extern void sched_timer_mgr_destroy(sched_timer_mgr_t* mgr);

/**
 * @brief Process expired timers.
 *
 * Fires callbacks for all timers whose deadline <= now_ms.
 * Must be called periodically (e.g. after each poll wait).
 *
 * @param mgr     Manager handle.
 * @param now_ms  Current time in milliseconds.
 *
 * @return Next timeout in ms until the earliest timer, or -1 if none.
 */
extern int sched_timer_mgr_process(sched_timer_mgr_t* mgr, uint64_t now_ms);

/**
 * @brief Get the timeout until the next timer fires.
 *
 * @param mgr     Manager handle.
 * @param now_ms  Current time in milliseconds.
 *
 * @return Timeout in ms, or -1 if no timers are active.
 */
extern int sched_timer_mgr_next_timeout(
    sched_timer_mgr_t* mgr, uint64_t now_ms);

/**
 * @brief Create a timer.
 *
 * @param mgr  Manager that owns this timer.
 *
 * @return Timer handle, or NULL on failure.
 */
extern sched_timer_t* sched_timer_create(sched_timer_mgr_t* mgr);

/**
 * @brief Destroy a timer. Stops it first if active.
 *
 * @param timer  Timer handle.
 */
extern void sched_timer_destroy(sched_timer_t* timer);

/**
 * @brief Start or restart a timer.
 *
 * Thread-safe.
 *
 * @param timer       Timer handle.
 * @param cb          Callback to invoke on expiry.
 * @param ud          User data for callback.
 * @param timeout_ms  Delay in milliseconds.
 * @param repeat_ms   Repeat interval, 0 for one-shot.
 */
extern void sched_timer_start(
    sched_timer_t*   timer,
    sched_timer_fn_t cb,
    void*            ud,
    uint64_t         timeout_ms,
    uint64_t         repeat_ms);

/**
 * @brief Stop a running timer.
 *
 * Thread-safe. No-op if already stopped.
 *
 * @param timer  Timer handle.
 */
extern void sched_timer_stop(sched_timer_t* timer);
