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

typedef struct loop_s       loop_t;
typedef struct loop_timer_s loop_timer_t;
typedef struct loop_io_s    loop_io_t;
typedef struct loop_post_s  loop_post_t;

/* Platform-neutral file descriptor (int on Unix, SOCKET on Windows). */
typedef intptr_t loop_poller_fd_t;

/* I/O event interest / readiness mask. */
typedef enum loop_poller_op_e {
    LOOP_POLLER_NO_OP = 0,
    LOOP_POLLER_RD_OP = 1,
    LOOP_POLLER_WR_OP = 2,
    LOOP_POLLER_RW_OP = 3,
} loop_poller_op_t;

/* Timer expiry callback. */
typedef void (*loop_timer_fn_t)(loop_t* loop,
                                      loop_timer_t* timer,
                                      void* ud);

/* Deferred (posted) callback. */
typedef void (*loop_post_fn_t)(loop_t* loop,
                                     loop_post_t* req,
                                     void* ud);

/**
 * @brief Create an event loop.
 *
 * Allocates and initializes the internal poller, timer heap, and
 * wakeup socketpair.
 *
 * @return Loop handle, or NULL on failure.
 */
extern loop_t* loop_create(void);

/**
 * @brief Destroy an event loop and release all resources.
 *
 * All handles must be closed before calling destroy.
 *
 * @param loop  Loop handle.
 */
extern void loop_destroy(loop_t* loop);

/**
 * @brief Run the event loop.
 *
 * Blocks until there are no more active handles or loop_stop()
 * is called. Each iteration polls for I/O, processes expired timers,
 * and drains the post queue.
 *
 * @param loop  Loop handle.
 *
 * @return 0 on normal exit, -1 on error.
 */
extern int loop_run(loop_t* loop);

/**
 * @brief Stop the event loop.
 *
 * Thread-safe. The loop will exit on the next iteration.
 *
 * @param loop  Loop handle.
 */
extern void loop_stop(loop_t* loop);

/**
 * @brief Create a timer handle.
 *
 * Does not start the timer. Call loop_start_timer() to begin.
 *
 * @param loop  Loop handle.
 *
 * @return Timer handle, or NULL on failure.
 */
extern loop_timer_t* loop_create_timer(loop_t* loop);

/**
 * @brief Destroy a timer handle.
 *
 * Stops the timer if active and frees the handle. Decrements the
 * loop active handle count. The handle must not be used after
 * this call.
 *
 * @param timer  Timer handle.
 */
extern void loop_destroy_timer(loop_timer_t* timer);

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
extern int loop_start_timer(loop_timer_t* timer,
                                  loop_timer_fn_t cb,
                                  void* ud,
                                  uint64_t timeout_ms,
                                  uint64_t repeat_ms);

/**
 * @brief Stop a running timer.
 *
 * Removes the timer from the heap. The handle remains valid and can
 * be re-started with loop_start_timer().
 *
 * @param timer  Timer handle.
 *
 * @return 0 on success, -1 on failure.
 */
extern int loop_stop_timer(loop_timer_t* timer);

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
extern int loop_reset_timer(loop_timer_t* timer,
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
extern int loop_post(loop_t* loop,
                           loop_post_fn_t cb,
                           void* ud);

/**
 * @brief Check if the caller is on the loop thread.
 *
 * Compares the calling thread's ID against the thread that entered
 * loop_run(). Only valid after loop_run() has been called.
 *
 * @param loop  Loop handle.
 *
 * @return true if called from the loop thread, false otherwise.
 */
extern bool loop_is_owner(loop_t* loop);


/**
 * @brief Callback invoked when an I/O event fires.
 *
 * @param loop     Loop handle.
 * @param io       I/O handle that triggered.
 * @param revents  Event mask (LOOP_POLLER_RD_OP, WR_OP, or both).
 * @param ud       User data pointer from loop_start_io().
 */
typedef void (*loop_io_fn_t)(loop_t* loop,
                                   loop_io_t* io,
                                   loop_poller_op_t revents,
                                   void* ud);

/**
 * @brief Create an I/O handle and bind it to a file descriptor.
 *
 * Does not start polling. Call loop_start_io() to begin.
 *
 * @param loop  Loop handle.
 * @param fd    File descriptor (socket) to monitor.
 *
 * @return I/O handle, or NULL on failure.
 */
extern loop_io_t* loop_create_io(loop_t* loop,
                                             loop_poller_fd_t fd);

/**
 * @brief Destroy an I/O handle.
 *
 * Stops polling if active and frees the handle. Decrements the
 * loop active handle count.
 *
 * @param io  I/O handle.
 */
extern void loop_destroy_io(loop_io_t* io);

/**
 * @brief Start or update polling on an I/O handle.
 *
 * Registers the fd in the poller with the given operation mask.
 *
 * @param io  I/O handle.
 * @param op  Event interest (LOOP_POLLER_RD_OP, WR_OP, or both).
 * @param cb  Callback invoked when the event fires.
 * @param ud  User data pointer passed to the callback.
 *
 * @return 0 on success, -1 on failure.
 */
extern int loop_start_io(loop_io_t* io,
                               loop_poller_op_t op,
                               loop_io_fn_t cb,
                               void* ud);

/**
 * @brief Stop polling on an I/O handle.
 *
 * Removes the fd from the poller. The handle remains valid and can
 * be re-started with loop_start_io().
 *
 * @param io  I/O handle.
 *
 * @return 0 on success, -1 on failure.
 */
extern int loop_stop_io(loop_io_t* io);
