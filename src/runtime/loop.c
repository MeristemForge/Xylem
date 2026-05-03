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

#include "runtime/loop.h"
#include "container/heap.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-utils.h"

#include "platform/platform.h"
#include "container/mpsc.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct loop_s {
    platform_poller_sq_t  poller;
    heap_t          timers;
    mpsc_t                posts;
    platform_sock_t       wakeup_rd;
    platform_sock_t       wakeup_wr;
    platform_poller_sqe_t wakeup_sqe;
    size_t                active_count;
    uint64_t              time;
    _Atomic bool          stopped;
    platform_tid_t        tid;
};

struct loop_io_s {
    platform_poller_sqe_t sqe;
    loop_t*         loop;
    loop_io_fn_t    cb;
    void*                 ud;
    bool                  registered;
};

struct loop_timer_s {
    heap_node_t     heap_node;
    loop_t*         loop;
    loop_timer_fn_t cb;
    void*                 ud;
    uint64_t              timeout;
    uint64_t              repeat;
    bool                  active;
};

struct loop_post_s {
    mpsc_node_t          node;
    loop_post_fn_t cb;
    void*                ud;
};

static int
_loop_cmp_timer(const heap_node_t* a, const heap_node_t* b) {
    const loop_timer_t* ta =
        heap_entry(a, loop_timer_t, heap_node);
    const loop_timer_t* tb =
        heap_entry(b, loop_timer_t, heap_node);
    if (ta->timeout < tb->timeout) {
        return -1;
    }
    if (ta->timeout > tb->timeout) {
        return 1;
    }
    return 0;
}

static void _loop_update_time(loop_t* loop) {
    loop->time = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
}

static void _loop_drain_wakeup(loop_t* loop) {
    char buf[64];
    for (;;) {
        ssize_t n = platform_socket_recv(loop->wakeup_rd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
    }
}

static void _loop_process_posts(loop_t* loop) {
    mpsc_node_t* node;
    while ((node = mpsc_pop(&loop->posts)) != NULL) {
        loop_post_t* req = mpsc_entry(node, loop_post_t, node);
        req->cb(loop, req, req->ud);
        free(req);
    }
}

static void _loop_process_timers(loop_t* loop) {
    for (;;) {
        heap_node_t* root = heap_peek(&loop->timers);
        if (!root) {
            break;
        }
        loop_timer_t* timer =
            heap_entry(root, loop_timer_t, heap_node);
        if (timer->timeout > loop->time) {
            break;
        }
        heap_dequeue(&loop->timers);
        if (timer->repeat > 0) {
            timer->timeout = loop->time + timer->repeat;
            heap_insert(&loop->timers, &timer->heap_node);
        } else {
            timer->active = false;
        }
        timer->cb(loop, timer, timer->ud);
    }
}

static int _loop_next_timeout(loop_t* loop) {
    heap_node_t* root = heap_peek(&loop->timers);
    if (!root) {
        return -1;
    }
    loop_timer_t* timer =
        heap_entry(root, loop_timer_t, heap_node);
    if (timer->timeout <= loop->time) {
        return 0;
    }
    uint64_t diff = timer->timeout - loop->time;
    if (diff > INT32_MAX) {
        return INT32_MAX;
    }
    return (int)diff;
}

loop_t* loop_create(void) {
    platform_socket_startup();

    loop_t* loop = (loop_t*)calloc(1, sizeof(loop_t));
    if (!loop) {
        return NULL;
    }

    if (platform_poller_init(&loop->poller) != 0) {
        free(loop);
        return NULL;
    }

    heap_init(&loop->timers, _loop_cmp_timer);
    mpsc_init(&loop->posts);

    platform_sock_t pair[2];
    if (platform_socket_socketpair(0, SOCK_STREAM, 0, pair) != 0) {
        platform_poller_destroy(&loop->poller);
        free(loop);
        return NULL;
    }

    loop->wakeup_rd = pair[0];
    loop->wakeup_wr = pair[1];

    platform_socket_enable_nonblocking(loop->wakeup_rd, true);
    platform_socket_enable_nonblocking(loop->wakeup_wr, true);

    memset(&loop->wakeup_sqe, 0, sizeof(loop->wakeup_sqe));
    loop->wakeup_sqe.fd = loop->wakeup_rd;
    loop->wakeup_sqe.op = PLATFORM_POLLER_RD_OP;
    loop->wakeup_sqe.ud = NULL;

    if (platform_poller_add(&loop->poller, &loop->wakeup_sqe) != 0) {
        platform_socket_close(loop->wakeup_rd);
        platform_socket_close(loop->wakeup_wr);
        platform_poller_destroy(&loop->poller);
        free(loop);
        return NULL;
    }

    loop->active_count = 0;
    atomic_store(&loop->stopped, false);
    _loop_update_time(loop);

    xylem_logi("loop create ok");
    return loop;
}

void loop_destroy(loop_t* loop) {
    if (!loop) {
        return;
    }

    xylem_logi("loop destroy");

    /* Drain pending posts -- invoke callbacks so deferred frees
     * (IO objects, conn structs, etc.) are released.  Loop until
     * stable because a callback may enqueue new posts. */
    for (;;) {
        bool drained = false;
        mpsc_node_t* node;
        while ((node = mpsc_pop(&loop->posts)) != NULL) {
            drained = true;
            loop_post_t* req =
                mpsc_entry(node, loop_post_t, node);
            req->cb(loop, req, req->ud);
            free(req);
        }
        if (!drained) {
            break;
        }
    }

    platform_poller_del(&loop->poller, &loop->wakeup_sqe);
    platform_socket_close(loop->wakeup_rd);
    platform_socket_close(loop->wakeup_wr);
    platform_poller_destroy(&loop->poller);
    free(loop);

    platform_socket_cleanup();
}

int loop_run(loop_t* loop) {
    platform_poller_cqe_t cqes[PLATFORM_POLLER_CQE_NUM];

    loop->tid = platform_info_gettid();
    atomic_store(&loop->stopped, false);
    _loop_update_time(loop);

    /* Drain posts queued before loop_run (e.g. coro spawns). */
    _loop_process_posts(loop);

    while ((loop->active_count > 0 || !mpsc_empty(&loop->posts)) &&
           !atomic_load(&loop->stopped)) {
        int timeout = _loop_next_timeout(loop);
        int n = platform_poller_wait(&loop->poller, cqes, timeout);

        _loop_update_time(loop);

        /* process I/O completions */
        for (int i = 0; i < n; i++) {
            if (cqes[i].ud == NULL) {
                _loop_drain_wakeup(loop);
                continue;
            }
            loop_io_t* io = cqes[i].ud;
            /**
             * IO may have been destroyed by a prior callback in this
             * batch -- cb is set to NULL by loop_destroy_io().
             */
            if (io->cb == NULL) {
                continue;
            }
            io->cb(io->loop, io, (loop_poller_op_t)cqes[i].op, io->ud);
        }

        _loop_process_posts(loop);
        _loop_process_timers(loop);
        /* process posts again -- timers may have called post() inline */
        _loop_process_posts(loop);
    }

    return 0;
}

void loop_stop(loop_t* loop) {
    xylem_logi("loop stop requested");
    atomic_store(&loop->stopped, true);
    char c = 1;
    platform_socket_send(loop->wakeup_wr, &c, 1);
}

loop_io_t*
loop_create_io(loop_t* loop, loop_poller_fd_t fd) {
    loop_io_t* io = (loop_io_t*)calloc(1, sizeof(loop_io_t));
    if (!io) {
        return NULL;
    }
    io->loop = loop;
    io->sqe.fd = (platform_poller_fd_t)fd;
    io->sqe.ud = io;
    io->sqe.oneshot = 1;
    io->ud = NULL;
    io->registered = false;
    io->cb = NULL;
    loop->active_count++;
    return io;
}

static void _loop_io_free_cb(loop_t* loop,
                             loop_post_t* req,
                             void* ud) {
    (void)loop;
    (void)req;
    free(ud);
}

void loop_destroy_io(loop_io_t* io) {
    if (!io) {
        return;
    }
    if (io->registered) {
        loop_stop_io(io);
    }
    io->loop->active_count--;

    /**
     * Mark the IO as dead so the dispatch loop in loop_run()
     * skips it if this IO was returned by the current poll batch.
     * Defer the actual free to the next post-processing pass so the
     * pointer remains valid for the remainder of the current iteration.
     */
    io->cb = NULL;
    loop_post(io->loop, _loop_io_free_cb, io);
}

int loop_start_io(
    loop_io_t* io, loop_poller_op_t op, loop_io_fn_t cb,
    void* ud) {
    io->sqe.op = (platform_poller_op_t)op;
    io->cb = cb;
    io->ud = ud;
    int rc;
    if (!io->registered) {
        rc = platform_poller_add(&io->loop->poller, &io->sqe);
        if (rc == 0) {
            io->registered = true;
        }
    } else {
        rc = platform_poller_mod(&io->loop->poller, &io->sqe);
    }
    return rc;
}

int loop_stop_io(loop_io_t* io) {
    if (!io->registered) {
        return 0;
    }
    int rc = platform_poller_del(&io->loop->poller, &io->sqe);
    if (rc == 0) {
        io->registered = false;
    }
    return rc;
}

loop_timer_t* loop_create_timer(loop_t* loop) {
    loop_timer_t* timer = (loop_timer_t*)calloc(1, sizeof(loop_timer_t));
    if (!timer) {
        return NULL;
    }
    timer->loop = loop;
    timer->ud = NULL;
    timer->active = false;
    timer->cb = NULL;
    loop->active_count++;
    return timer;
}

void loop_destroy_timer(loop_timer_t* timer) {
    if (!timer) {
        return;
    }
    if (timer->active) {
        loop_stop_timer(timer);
    }
    timer->loop->active_count--;
    free(timer);
}

int loop_start_timer(
    loop_timer_t*   timer,
    loop_timer_fn_t cb,
    void*                 ud,
    uint64_t              timeout_ms,
    uint64_t              repeat_ms) {
    if (timer->active) {
        heap_remove(&timer->loop->timers, &timer->heap_node);
    }
    timer->cb = cb;
    timer->ud = ud;
    timer->timeout = timer->loop->time + timeout_ms;
    timer->repeat = repeat_ms;
    timer->active = true;
    heap_insert(&timer->loop->timers, &timer->heap_node);
    return 0;
}

int loop_stop_timer(loop_timer_t* timer) {
    if (!timer->active) {
        return 0;
    }
    heap_remove(&timer->loop->timers, &timer->heap_node);
    timer->active = false;
    return 0;
}

int loop_reset_timer(loop_timer_t* timer, uint64_t timeout_ms) {
    if (!timer->active) {
        return -1;
    }
    heap_remove(&timer->loop->timers, &timer->heap_node);
    timer->timeout = timer->loop->time + timeout_ms;
    heap_insert(&timer->loop->timers, &timer->heap_node);
    return 0;
}

int loop_post(loop_t* loop, loop_post_fn_t cb, void* ud) {
    loop_post_t* req = (loop_post_t*)calloc(1, sizeof(loop_post_t));
    if (!req) {
        return -1;
    }
    req->cb = cb;
    req->ud = ud;
    mpsc_push(&loop->posts, &req->node);
    if (!loop_is_owner(loop)) {
        char    c = 1;
        ssize_t n = platform_socket_send(loop->wakeup_wr, &c, 1);
        if (n <= 0) {
            xylem_logw("loop post: wakeup send failed, task queued anyway");
        }
    }
    return 0;
}

bool loop_is_owner(loop_t* loop) {
    return platform_info_gettid() == loop->tid;
}
