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

#include "xylem/xylem-loop.h"
#include "xylem/xylem-logger.h"

/* timer comparator: earlier deadline = higher priority */
static int _loop_timer_cmp(const xylem_heap_node_t* a,
                           const xylem_heap_node_t* b) {
    const xylem_loop_timer_t* ta =
        xylem_heap_entry(a, xylem_loop_timer_t, heap_node);
    const xylem_loop_timer_t* tb =
        xylem_heap_entry(b, xylem_loop_timer_t, heap_node);

    if (ta->timeout < tb->timeout) return -1;
    if (ta->timeout > tb->timeout) return  1;
    return 0;
}

static void _loop_update_time(xylem_loop_t* loop) {
    loop->time = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
}

/* drain the wakeup socketpair so it doesn't keep firing */
static void _loop_drain_wakeup(xylem_loop_t* loop) {
    char buf[64];
    for (;;) {
        ssize_t n = platform_socket_recv(loop->wakeup_rd, buf, sizeof(buf));
        if (n <= 0) break;
    }
}

/* process all pending post requests */
static void _loop_process_posts(xylem_loop_t* loop) {
    /* Fast path: skip lock when queue is empty.
     * A concurrent enqueue between this check and the lock is fine —
     * the wakeup write will trigger another iteration. */
    if (xylem_queue_empty(&loop->posts)) return;

    xylem_queue_t local;
    xylem_queue_init(&local);

    /* swap under lock to minimize hold time */
    mtx_lock(&loop->post_mtx);
    xylem_queue_swap(&loop->posts, &local);
    mtx_unlock(&loop->post_mtx);

    while (!xylem_queue_empty(&local)) {
        xylem_queue_node_t* node = xylem_queue_dequeue(&local);
        xylem_loop_post_t*  req  =
            xylem_queue_entry(node, xylem_loop_post_t, node);
        req->cb(loop, req);
    }
}

/* fire all expired timers */
static void _loop_process_timers(xylem_loop_t* loop) {
    for (;;) {
        xylem_heap_node_t* root = xylem_heap_root(&loop->timers);
        if (!root) break;

        xylem_loop_timer_t* timer =
            xylem_heap_entry(root, xylem_loop_timer_t, heap_node);

        if (timer->timeout > loop->time) break;

        xylem_heap_dequeue(&loop->timers);

        if (timer->repeat > 0) {
            timer->timeout = loop->time + timer->repeat;
            xylem_heap_insert(&loop->timers, &timer->heap_node);
        } else {
            timer->active = false;
        }
        timer->cb(loop, timer);
    }
}

/* process handles queued for close */
static void _loop_process_closing(xylem_loop_t* loop) {
    while (!xylem_queue_empty(&loop->closing)) {
        xylem_queue_node_t* node = xylem_queue_dequeue(&loop->closing);
        (void)node;
        loop->active_count--;
    }
}

/* calculate timeout for poller_wait from nearest timer */
static int _loop_next_timeout(xylem_loop_t* loop) {
    xylem_heap_node_t* root = xylem_heap_root(&loop->timers);
    if (!root) return -1;

    xylem_loop_timer_t* timer =
        xylem_heap_entry(root, xylem_loop_timer_t, heap_node);

    if (timer->timeout <= loop->time) return 0;

    uint64_t diff = timer->timeout - loop->time;
    if (diff > INT32_MAX) return INT32_MAX;
    return (int)diff;
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

int xylem_loop_init(xylem_loop_t* loop) {
    memset(loop, 0, sizeof(*loop));

    if (platform_poller_init(&loop->poller) != 0) {
        xylem_loge("loop init: poller init failed");
        return -1;
    }
    xylem_heap_init(&loop->timers, _loop_timer_cmp);
    xylem_queue_init(&loop->closing);
    xylem_queue_init(&loop->posts);

    if (mtx_init(&loop->post_mtx, mtx_plain) != thrd_success) {
        xylem_loge("loop init: mutex init failed");
        platform_poller_destroy(&loop->poller);
        return -1;
    }

    platform_sock_t pair[2];
    if (platform_socket_socketpair(0, SOCK_STREAM, 0, pair) != 0) {
        xylem_loge("loop init: socketpair failed");
        mtx_destroy(&loop->post_mtx);
        platform_poller_destroy(&loop->poller);
        return -1;
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
        xylem_loge("loop init: wakeup fd poller_add failed");
        platform_socket_close(loop->wakeup_rd);
        platform_socket_close(loop->wakeup_wr);
        mtx_destroy(&loop->post_mtx);
        platform_poller_destroy(&loop->poller);
        return -1;
    }
    loop->active_count = 0;
    atomic_store(&loop->stopped, false);
    _loop_update_time(loop);

    xylem_logd("loop init ok");
    return 0;
}

void xylem_loop_destroy(xylem_loop_t* loop) {
    xylem_logd("loop destroy");
    platform_poller_del(&loop->poller, &loop->wakeup_sqe);
    platform_socket_close(loop->wakeup_rd);
    platform_socket_close(loop->wakeup_wr);
    mtx_destroy(&loop->post_mtx);
    platform_poller_destroy(&loop->poller);
}

int xylem_loop_run(xylem_loop_t* loop) {
    platform_poller_cqe_t cqes[PLATFORM_POLLER_CQE_NUM];

    atomic_store(&loop->stopped, false);
    _loop_update_time(loop);

    while (loop->active_count > 0 && !atomic_load(&loop->stopped)) {
        int timeout = _loop_next_timeout(loop);
        int n = platform_poller_wait(&loop->poller, cqes, timeout);

        _loop_update_time(loop);

        /* process I/O completions */
        for (int i = 0; i < n; i++) {
            if (cqes[i].ud == NULL) {
                /* wakeup fd fired — drain so LT doesn't keep firing */
                _loop_drain_wakeup(loop);
                continue;
            }
            xylem_loop_io_t* io = cqes[i].ud;
            io->cb(io->loop, io, cqes[i].op);
        }

        _loop_process_posts(loop);
        _loop_process_timers(loop);
        /* process posts again — timers may have called post() inline */
        _loop_process_posts(loop);
        _loop_process_closing(loop);
    }
    return 0;
}

void xylem_loop_stop(xylem_loop_t* loop) {
    xylem_logd("loop stop requested");
    atomic_store(&loop->stopped, true);
    /* wake up the poller in case it's blocked */
    char c = 1;
    platform_socket_send(loop->wakeup_wr, &c, 1);
}

uint64_t xylem_loop_now(xylem_loop_t* loop) {
    return loop->time;
}

/* ------------------------------------------------------------------ */
/*  I/O                                                               */
/* ------------------------------------------------------------------ */

int xylem_loop_io_init(xylem_loop_t* loop,
                       xylem_loop_io_t* io,
                       platform_poller_fd_t fd) {
    memset(io, 0, sizeof(*io));
    io->loop       = loop;
    io->sqe.fd     = fd;
    io->sqe.ud     = io;
    io->registered = false;
    io->cb         = NULL;
    loop->active_count++;
    return 0;
}

int xylem_loop_io_start(xylem_loop_io_t* io,
                        platform_poller_op_t op,
                        xylem_loop_io_fn_t cb) {
    io->sqe.op = op;
    io->cb     = cb;

    int rc;
    if (!io->registered) {
        rc = platform_poller_add(&io->loop->poller, &io->sqe);
        if (rc == 0) {
            io->registered = true;
            xylem_logd("loop io_start: add fd=%d op=%d",
                       (int)io->sqe.fd, (int)op);
        }
    } else {
        rc = platform_poller_mod(&io->loop->poller, &io->sqe);
        if (rc == 0) {
            xylem_logd("loop io_start: mod fd=%d op=%d",
                       (int)io->sqe.fd, (int)op);
        }
    }
    return rc;
}

int xylem_loop_io_stop(xylem_loop_io_t* io) {
    if (!io->registered) return 0;

    int rc = platform_poller_del(&io->loop->poller, &io->sqe);
    if (rc == 0) {
        xylem_logd("loop io_stop: del fd=%d", (int)io->sqe.fd);
        io->registered = false;
    }
    return rc;
}

void xylem_loop_io_close(xylem_loop_io_t* io) {
    if (io->registered) {
        platform_poller_del(&io->loop->poller, &io->sqe);
        io->registered = false;
    }
    xylem_queue_enqueue(&io->loop->closing, &io->close_node);
}

/* ------------------------------------------------------------------ */
/*  Timer                                                             */
/* ------------------------------------------------------------------ */

int xylem_loop_timer_init(xylem_loop_t* loop,
                          xylem_loop_timer_t* timer) {
    memset(timer, 0, sizeof(*timer));
    timer->loop   = loop;
    timer->active = false;
    timer->cb     = NULL;
    loop->active_count++;
    return 0;
}

int xylem_loop_timer_start(xylem_loop_timer_t* timer,
                           xylem_loop_timer_fn_t cb,
                           uint64_t timeout_ms,
                           uint64_t repeat_ms) {
    if (timer->active) {
        xylem_heap_remove(&timer->loop->timers, &timer->heap_node);
    }
    timer->cb      = cb;
    timer->timeout = timer->loop->time + timeout_ms;
    timer->repeat  = repeat_ms;
    timer->active  = true;
    xylem_heap_insert(&timer->loop->timers, &timer->heap_node);
    return 0;
}

int xylem_loop_timer_stop(xylem_loop_timer_t* timer) {
    if (!timer->active) return 0;

    xylem_heap_remove(&timer->loop->timers, &timer->heap_node);
    timer->active = false;
    return 0;
}

int xylem_loop_timer_reset(xylem_loop_timer_t* timer,
                           uint64_t timeout_ms) {
    if (!timer->active) return -1;

    xylem_heap_remove(&timer->loop->timers, &timer->heap_node);
    timer->timeout = timer->loop->time + timeout_ms;
    xylem_heap_insert(&timer->loop->timers, &timer->heap_node);
    return 0;
}

void xylem_loop_timer_close(xylem_loop_timer_t* timer) {
    if (timer->active) {
        xylem_heap_remove(&timer->loop->timers, &timer->heap_node);
        timer->active = false;
    }
    xylem_queue_enqueue(&timer->loop->closing, &timer->close_node);
}

/* ------------------------------------------------------------------ */
/*  Post (thread-safe)                                                */
/* ------------------------------------------------------------------ */

int xylem_loop_post(xylem_loop_t* loop, xylem_loop_post_t* req) {
    mtx_lock(&loop->post_mtx);
    xylem_queue_enqueue(&loop->posts, &req->node);
    mtx_unlock(&loop->post_mtx);

    char c = 1;
    ssize_t n = platform_socket_send(loop->wakeup_wr, &c, 1);
    return (n > 0) ? 0 : -1;
}
