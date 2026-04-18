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

/* Opaque event loop handle. */
typedef struct xylem_loop_s       xylem_loop_t;
/* Opaque timer handle. */
typedef struct xylem_loop_timer_s xylem_loop_timer_t;
/* Opaque deferred-callback handle. */
typedef struct xylem_loop_post_s  xylem_loop_post_t;

/* Timer expiry callback. */
typedef void (*xylem_loop_timer_fn_t)(xylem_loop_t* loop,
                                      xylem_loop_timer_t* timer,
                                      void* ud);

/* Deferred (posted) callback. */
typedef void (*xylem_loop_post_fn_t)(xylem_loop_t* loop,
                                     xylem_loop_post_t* req,
                                     void* ud);

/**
 * @brief Create an event loop.
 *
 * Allocates and initializes the internal poller, timer heap, and
 * wakeup socketpair.
 *
 * @return Loop handle, or NULL on failure.
 */
extern xylem_loop_t* xylem_loop_create(void);

/**
 * @brief Destroy an event loop and release all resources.
 *
 * All handles must be closed before calling destroy.
 *
 * @param loop  Loop handle.
 */
extern void xylem_loop_destroy(xylem_loop_t* loop);

/**
 * @brief Run the event loop.
 *
 * Blocks until there are no more active handles or xylem_loop_stop()
 * is called. Each iteration polls for I/O, processes expired timers,
 * and drains the post queue.
 *
 * @param loop  Loop handle.
 *
 * @return 0 on normal exit, -1 on error.
 */
extern int xylem_loop_run(xylem_loop_t* loop);

/**
 * @brief Stop the event loop.
 *
 * Thread-safe. The loop will exit on the next iteration.
 *
 * @param loop  Loop handle.
 */
extern void xylem_loop_stop(xylem_loop_t* loop);

/**
 * @brief Create a timer handle.
 *
 * Does not start the timer. Call xylem_loop_start_timer() to begin.
 *
 * @param loop  Loop handle.
 *
 * @return Timer handle, or NULL on failure.
 */
extern xylem_loop_timer_t* xylem_loop_create_timer(xylem_loop_t* loop);

/**
 * @brief Destroy a timer handle.
 *
 * Stops the timer if active and frees the handle. Decrements the
 * loop active handle count. The handle must not be used after
 * this call.
 *
 * @param timer  Timer handle.
 */
extern void xylem_loop_destroy_timer(xylem_loop_timer_t* timer);

/**
 * @brief Start a timer.
 *
 * The callback fires after timeout_ms milliseconds. If repeat_ms > 0,
 * the timer re-arms automatically with that interval. If repeat_ms == 0,
 * the timer is one-shot.
 *
 * @param timer       Timer handle.
 * @param cb          Callback invoked on expiry.
 * @param ud          User data pointer passed to the callback.
 * @param timeout_ms  Initial delay in milliseconds.
 * @param repeat_ms   Repeat interval in milliseconds (0 for one-shot).
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_loop_start_timer(xylem_loop_timer_t* timer,
                                  xylem_loop_timer_fn_t cb,
                                  void* ud,
                                  uint64_t timeout_ms,
                                  uint64_t repeat_ms);

/**
 * @brief Stop a running timer.
 *
 * Removes the timer from the heap. The handle remains valid and can
 * be re-started with xylem_loop_start_timer().
 *
 * @param timer  Timer handle.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_loop_stop_timer(xylem_loop_timer_t* timer);

/**
 * @brief Reset a running timer with a new timeout.
 *
 * Equivalent to stop + start with the same callback and repeat,
 * but avoids a redundant heap remove/insert when possible.
 *
 * @param timer       Timer handle.
 * @param timeout_ms  New delay in milliseconds from now.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_loop_reset_timer(xylem_loop_timer_t* timer,
                                  uint64_t timeout_ms);

/**
 * @brief Post a callback to be executed on the loop thread.
 *
 * Thread-safe. The loop allocates an internal node and enqueues it.
 * The callback is invoked on the next loop iteration with the
 * provided user data.
 *
 * @param loop  Loop handle.
 * @param cb    Callback to invoke on the loop thread.
 * @param ud    User data pointer passed to the callback.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_loop_post(xylem_loop_t* loop,
                           xylem_loop_post_fn_t cb,
                           void* ud);
