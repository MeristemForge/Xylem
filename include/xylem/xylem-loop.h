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

#include "xylem.h"
#include "platform/platform.h"

/* ------------------------------------------------------------------ */
/*  Forward declarations                                              */
/* ------------------------------------------------------------------ */

typedef struct xylem_loop_s       xylem_loop_t;
typedef struct xylem_loop_io_s    xylem_loop_io_t;
typedef struct xylem_loop_timer_s xylem_loop_timer_t;
typedef struct xylem_loop_post_s  xylem_loop_post_t;

/* ------------------------------------------------------------------ */
/*  Callback typedefs                                                 */
/* ------------------------------------------------------------------ */

typedef void (*xylem_loop_io_fn_t)(xylem_loop_t* loop,
                                   xylem_loop_io_t* io,
                                   platform_poller_op_t revents);

typedef void (*xylem_loop_timer_fn_t)(xylem_loop_t* loop,
                                      xylem_loop_timer_t* timer);

typedef void (*xylem_loop_post_fn_t)(xylem_loop_t* loop,
                                     xylem_loop_post_t* req);

/* ------------------------------------------------------------------ */
/*  Structures                                                        */
/* ------------------------------------------------------------------ */

struct xylem_loop_s {
    platform_poller_sq_t  poller;
    xylem_heap_t          timers;         /* timer min-heap */
    xylem_queue_t         closing;        /* handles pending close */
    xylem_queue_t         posts;          /* post queue, drained on loop thread */
    platform_sock_t       wakeup_rd;      /* socketpair read end, registered in poller */
    platform_sock_t       wakeup_wr;      /* socketpair write end, written by post() */
    platform_poller_sqe_t wakeup_sqe;     /* poller registration for wakeup_rd */
    mtx_t                 post_mtx;       /* protects posts queue */
    size_t                active_count;   /* active handles, run exits when 0 */
    uint64_t              time;           /* cached monotonic time (ms) */
    _Atomic bool          stopped;
};

struct xylem_loop_io_s {
    platform_poller_sqe_t sqe;
    xylem_loop_t*         loop;
    xylem_loop_io_fn_t    cb;
    xylem_queue_node_t    close_node;
    bool                  registered;     /* fd added to poller (peer socket exists) */
};

struct xylem_loop_timer_s {
    xylem_heap_node_t     heap_node;
    xylem_loop_t*         loop;
    xylem_loop_timer_fn_t cb;
    uint64_t              timeout;        /* absolute expiry time (ms) */
    uint64_t              repeat;         /* repeat interval, 0 = one-shot */
    xylem_queue_node_t    close_node;
    bool                  active;         /* currently in the timer heap */
};

struct xylem_loop_post_s {
    xylem_queue_node_t    node;
    xylem_loop_post_fn_t  cb;
    void*                 ud;
};

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialize an event loop.
 *
 * Creates the internal poller, timer heap, and wakeup socketpair.
 *
 * @param loop  Pointer to the loop structure to initialize.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_loop_init(xylem_loop_t* loop);

/**
 * @brief Destroy an event loop and release all resources.
 *
 * All handles must be closed before calling destroy.
 *
 * @param loop  Pointer to the loop.
 */
extern void xylem_loop_destroy(xylem_loop_t* loop);

/**
 * @brief Run the event loop.
 *
 * Blocks until there are no more active handles or xylem_loop_stop()
 * is called. Each iteration polls for I/O, processes expired timers,
 * drains the post queue, and processes the closing queue.
 *
 * @param loop  Pointer to the loop.
 *
 * @return 0 on normal exit, -1 on error.
 */
extern int xylem_loop_run(xylem_loop_t* loop);

/**
 * @brief Stop the event loop.
 *
 * Thread-safe. The loop will exit on the next iteration.
 *
 * @param loop  Pointer to the loop.
 */
extern void xylem_loop_stop(xylem_loop_t* loop);

/**
 * @brief Return the cached monotonic time in milliseconds.
 *
 * Updated once per loop iteration. Avoids repeated syscalls within
 * the same batch of callbacks.
 *
 * @param loop  Pointer to the loop.
 *
 * @return Monotonic timestamp in milliseconds.
 */
extern uint64_t xylem_loop_now(xylem_loop_t* loop);

/* ------------------------------------------------------------------ */
/*  I/O                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialize an I/O handle and bind it to a file descriptor.
 *
 * Does not start polling. Call xylem_loop_io_start() to begin.
 *
 * @param loop  Pointer to the loop.
 * @param io    Pointer to the I/O handle to initialize.
 * @param fd    File descriptor (socket) to monitor.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_loop_io_init(xylem_loop_t* loop,
                              xylem_loop_io_t* io,
                              platform_poller_fd_t fd);

/**
 * @brief Start or update polling on an I/O handle.
 *
 * Registers the fd in the poller with the given operation mask.
 * Uses level-triggered semantics: the callback fires as long as
 * the condition holds. Call stop to cancel.
 *
 * @param io  Pointer to the I/O handle.
 * @param op  Event interest (PLATFORM_POLLER_RD_OP, WR_OP, or RW_OP).
 * @param cb  Callback invoked when the event fires.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_loop_io_start(xylem_loop_io_t* io,
                               platform_poller_op_t op,
                               xylem_loop_io_fn_t cb);

/**
 * @brief Stop polling on an I/O handle.
 *
 * Removes the fd from the poller. The handle remains valid and can
 * be re-started with xylem_loop_io_start().
 *
 * @param io  Pointer to the I/O handle.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_loop_io_stop(xylem_loop_io_t* io);

/**
 * @brief Close an I/O handle.
 *
 * Stops polling if active and queues the handle for cleanup.
 * The handle must not be used after this call. Decrements the
 * loop active handle count.
 *
 * @param io  Pointer to the I/O handle.
 */
extern void xylem_loop_io_close(xylem_loop_io_t* io);

/* ------------------------------------------------------------------ */
/*  Timer                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialize a timer handle.
 *
 * Does not start the timer. Call xylem_loop_timer_start() to begin.
 *
 * @param loop   Pointer to the loop.
 * @param timer  Pointer to the timer handle to initialize.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_loop_timer_init(xylem_loop_t* loop,
                                 xylem_loop_timer_t* timer);

/**
 * @brief Start a timer.
 *
 * The callback fires after timeout_ms milliseconds. If repeat_ms > 0,
 * the timer re-arms automatically with that interval. If repeat_ms == 0,
 * the timer is one-shot.
 *
 * @param timer       Pointer to the timer handle.
 * @param cb          Callback invoked on expiry.
 * @param timeout_ms  Initial delay in milliseconds.
 * @param repeat_ms   Repeat interval in milliseconds (0 for one-shot).
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_loop_timer_start(xylem_loop_timer_t* timer,
                                  xylem_loop_timer_fn_t cb,
                                  uint64_t timeout_ms,
                                  uint64_t repeat_ms);

/**
 * @brief Stop a running timer.
 *
 * Removes the timer from the heap. The handle remains valid and can
 * be re-started with xylem_loop_timer_start().
 *
 * @param timer  Pointer to the timer handle.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_loop_timer_stop(xylem_loop_timer_t* timer);

/**
 * @brief Reset a running timer with a new timeout.
 *
 * Equivalent to stop + start with the same callback and repeat,
 * but avoids a redundant heap remove/insert when possible.
 *
 * @param timer       Pointer to the timer handle.
 * @param timeout_ms  New delay in milliseconds from now.
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_loop_timer_reset(xylem_loop_timer_t* timer,
                                  uint64_t timeout_ms);

/**
 * @brief Close a timer handle.
 *
 * Stops the timer if active and queues the handle for cleanup.
 * The handle must not be used after this call. Decrements the
 * loop active handle count.
 *
 * @param timer  Pointer to the timer handle.
 */
extern void xylem_loop_timer_close(xylem_loop_timer_t* timer);

/* ------------------------------------------------------------------ */
/*  Post (thread-safe)                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Post a callback to be executed on the loop thread.
 *
 * Thread-safe. The caller allocates the post request and sets cb/ud.
 * The request node must remain valid until the callback is invoked.
 * The loop is woken up via an internal socketpair.
 *
 * @param loop  Pointer to the loop.
 * @param req   Pointer to the post request (intrusive node).
 *
 * @return 0 on success, -1 on failure.
 */
extern int xylem_loop_post(xylem_loop_t* loop, xylem_loop_post_t* req);
