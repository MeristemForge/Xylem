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
#include "xylem/xylem-heap.h"
#include "xylem/xylem-logger.h"
#include "xylem/xylem-queue.h"
#include "xylem/xylem-utils.h"

#include "platform/platform.h"
#include "xylem-loop-io.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "deprecated/c11-threads.h"

struct xylem_loop_s {
    platform_poller_sq_t  poller;
    xylem_heap_t          timers;
    xylem_queue_t         posts;
    platform_sock_t       wakeup_rd;
    platform_sock_t       wakeup_wr;
    platform_poller_sqe_t wakeup_sqe;
    mtx_t                 post_mtx;
    size_t                active_count;
    uint64_t              time;
    _Atomic bool          stopped;
};

struct xylem_loop_io_s {
    platform_poller_sqe_t sqe;
    xylem_loop_t*         loop;
    xylem_loop_io_fn_t    cb;
    void*                 ud;
    bool                  registered;
};

struct xylem_loop_timer_s {
    xylem_heap_node_t     heap_node;
    xylem_loop_t*         loop;
    xylem_loop_timer_fn_t cb;
    void*                 ud;
    uint64_t              timeout;
    uint64_t              repeat;
    bool                  active;
};

struct xylem_loop_post_s {
    xylem_queue_node_t   node;
    xylem_loop_post_fn_t cb;
    void*                ud;
};

static int
_loop_cmp_timer(const xylem_heap_node_t* a, const xylem_heap_node_t* b) {
    const xylem_loop_timer_t* ta =
        xylem_heap_entry(a, xylem_loop_timer_t, heap_node);
    const xylem_loop_timer_t* tb =
        xylem_heap_entry(b, xylem_loop_timer_t, heap_node);
    if (ta->timeout < tb->timeout) {
        return -1;
    }
    if (ta->timeout > tb->timeout) {
        return 1;
    }
    return 0;
}

static void _loop_update_time(xylem_loop_t* loop) {
    loop->time = xylem_utils_getnow(XYLEM_TIME_PRECISION_MSEC);
}

static void _loop_drain_wakeup(xylem_loop_t* loop) {
    char buf[64];
    for (;;) {
        ssize_t n = platform_socket_recv(loop->wakeup_rd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
    }
}

static void _loop_process_posts(xylem_loop_t* loop) {
    xylem_queue_t local;
    xylem_queue_init(&local);

    mtx_lock(&loop->post_mtx);
    xylem_queue_swap(&loop->posts, &local);
    mtx_unlock(&loop->post_mtx);
    
    if (xylem_queue_empty(&local)) {
        return;
    }
    while (!xylem_queue_empty(&local)) {
        xylem_queue_node_t* node = xylem_queue_dequeue(&local);
        xylem_loop_post_t*  req =
            xylem_queue_entry(node, xylem_loop_post_t, node);
        req->cb(loop, req, req->ud);
        free(req);
    }
}

static void _loop_process_timers(xylem_loop_t* loop) {
    for (;;) {
        xylem_heap_node_t* root = xylem_heap_root(&loop->timers);
        if (!root) {
            break;
        }
        xylem_loop_timer_t* timer =
            xylem_heap_entry(root, xylem_loop_timer_t, heap_node);
        if (timer->timeout > loop->time) {
            break;
        }
        xylem_heap_dequeue(&loop->timers);
        if (timer->repeat > 0) {
            timer->timeout = loop->time + timer->repeat;
            xylem_heap_insert(&loop->timers, &timer->heap_node);
        } else {
            timer->active = false;
        }
        timer->cb(loop, timer, timer->ud);
    }
}

static int _loop_next_timeout(xylem_loop_t* loop) {
    xylem_heap_node_t* root = xylem_heap_root(&loop->timers);
    if (!root) {
        return -1;
    }
    xylem_loop_timer_t* timer =
        xylem_heap_entry(root, xylem_loop_timer_t, heap_node);
    if (timer->timeout <= loop->time) {
        return 0;
    }
    uint64_t diff = timer->timeout - loop->time;
    if (diff > INT32_MAX) {
        return INT32_MAX;
    }
    return (int)diff;
}

xylem_loop_t* xylem_loop_create(void) {
    xylem_loop_t* loop = calloc(1, sizeof(*loop));
    if (!loop) {
        return NULL;
    }

    if (platform_poller_init(&loop->poller) != 0) {
        free(loop);
        return NULL;
    }

    xylem_heap_init(&loop->timers, _loop_cmp_timer);
    xylem_queue_init(&loop->posts);

    if (mtx_init(&loop->post_mtx, mtx_plain) != thrd_success) {
        platform_poller_destroy(&loop->poller);
        free(loop);
        return NULL;
    }

    platform_sock_t pair[2];
    if (platform_socket_socketpair(0, SOCK_STREAM, 0, pair) != 0) {
        mtx_destroy(&loop->post_mtx);
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
        mtx_destroy(&loop->post_mtx);
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

void xylem_loop_destroy(xylem_loop_t* loop) {
    if (!loop) {
        return;
    }

    xylem_logi("loop destroy");

    /* Drain pending posts to avoid leaking queued nodes. */
    xylem_queue_node_t* node;
    mtx_lock(&loop->post_mtx);
    while ((node = xylem_queue_dequeue(&loop->posts)) != NULL) {
        free(xylem_queue_entry(node, xylem_loop_post_t, node));
    }
    mtx_unlock(&loop->post_mtx);

    platform_poller_del(&loop->poller, &loop->wakeup_sqe);
    platform_socket_close(loop->wakeup_rd);
    platform_socket_close(loop->wakeup_wr);
    mtx_destroy(&loop->post_mtx);
    platform_poller_destroy(&loop->poller);
    free(loop);
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
                _loop_drain_wakeup(loop);
                continue;
            }
            xylem_loop_io_t* io = cqes[i].ud;
            io->cb(io->loop, io, cqes[i].op, io->ud);
        }

        _loop_process_posts(loop);
        _loop_process_timers(loop);
        /* process posts again -- timers may have called post() inline */
        _loop_process_posts(loop);
    }

    return 0;
}

void xylem_loop_stop(xylem_loop_t* loop) {
    xylem_logi("loop stop requested");
    atomic_store(&loop->stopped, true);
    char c = 1;
    platform_socket_send(loop->wakeup_wr, &c, 1);
}

xylem_loop_io_t*
xylem_loop_create_io(xylem_loop_t* loop, platform_poller_fd_t fd) {
    xylem_loop_io_t* io = calloc(1, sizeof(*io));
    if (!io) {
        return NULL;
    }
    io->loop = loop;
    io->sqe.fd = fd;
    io->sqe.ud = io;
    io->ud = NULL;
    io->registered = false;
    io->cb = NULL;
    loop->active_count++;
    return io;
}

void xylem_loop_destroy_io(xylem_loop_io_t* io) {
    if (!io) {
        return;
    }
    if (io->registered) {
        xylem_loop_stop_io(io);
    }
    io->loop->active_count--;
    free(io);
}

int xylem_loop_start_io(
    xylem_loop_io_t* io, platform_poller_op_t op, xylem_loop_io_fn_t cb,
    void* ud) {
    io->sqe.op = op;
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

int xylem_loop_stop_io(xylem_loop_io_t* io) {
    if (!io->registered) {
        return 0;
    }
    int rc = platform_poller_del(&io->loop->poller, &io->sqe);
    if (rc == 0) {
        io->registered = false;
    }
    return rc;
}

xylem_loop_timer_t* xylem_loop_create_timer(xylem_loop_t* loop) {
    xylem_loop_timer_t* timer = calloc(1, sizeof(*timer));
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

void xylem_loop_destroy_timer(xylem_loop_timer_t* timer) {
    if (!timer) {
        return;
    }
    if (timer->active) {
        xylem_loop_stop_timer(timer);
    }
    timer->loop->active_count--;
    free(timer);
}

int xylem_loop_start_timer(
    xylem_loop_timer_t*   timer,
    xylem_loop_timer_fn_t cb,
    void*                 ud,
    uint64_t              timeout_ms,
    uint64_t              repeat_ms) {
    if (timer->active) {
        xylem_heap_remove(&timer->loop->timers, &timer->heap_node);
    }
    timer->cb = cb;
    timer->ud = ud;
    timer->timeout = timer->loop->time + timeout_ms;
    timer->repeat = repeat_ms;
    timer->active = true;
    xylem_heap_insert(&timer->loop->timers, &timer->heap_node);
    return 0;
}

int xylem_loop_stop_timer(xylem_loop_timer_t* timer) {
    if (!timer->active) {
        return 0;
    }
    xylem_heap_remove(&timer->loop->timers, &timer->heap_node);
    timer->active = false;
    return 0;
}

int xylem_loop_reset_timer(xylem_loop_timer_t* timer, uint64_t timeout_ms) {
    if (!timer->active) {
        return -1;
    }
    xylem_heap_remove(&timer->loop->timers, &timer->heap_node);
    timer->timeout = timer->loop->time + timeout_ms;
    xylem_heap_insert(&timer->loop->timers, &timer->heap_node);
    return 0;
}

int xylem_loop_post(xylem_loop_t* loop, xylem_loop_post_fn_t cb, void* ud) {
    xylem_loop_post_t* req = (xylem_loop_post_t*)calloc(1, sizeof(xylem_loop_post_t));
    if (!req) {
        return -1;
    }
    req->cb = cb;
    req->ud = ud;
    mtx_lock(&loop->post_mtx);
    xylem_queue_enqueue(&loop->posts, &req->node);
    mtx_unlock(&loop->post_mtx);
    char    c = 1;
    ssize_t n = platform_socket_send(loop->wakeup_wr, &c, 1);
    if (n <= 0) {
        xylem_logw("loop post: wakeup send failed, task queued anyway");
    }
    return 0;
}
