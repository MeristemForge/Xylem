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

#include "xylem/sync/xylem-waitgroup.h"

#include "runtime/runtime.h"
#include "minicoro/minicoro.h"

#include <stdatomic.h>
#include <stdlib.h>

struct xylem_waitgroup_s {
    atomic_size_t cnt;
    mco_coro*     wait_coro;
};

static void _waitgroup_wakeup_cb(
    loop_t* loop,
    loop_post_t* req,
    void* ud) {
    (void)loop;
    (void)req;
    xylem_waitgroup_t* wg = (xylem_waitgroup_t*)ud;
    if (wg->wait_coro) {
        mco_coro* co = wg->wait_coro;
        wg->wait_coro = NULL;
        mco_resume(co);
    }
}

xylem_waitgroup_t* xylem_waitgroup_create(void) {
    xylem_waitgroup_t* wg =
        (xylem_waitgroup_t*)calloc(1, sizeof(xylem_waitgroup_t));
    if (!wg) {
        return NULL;
    }
    atomic_init(&wg->cnt, 0);
    return wg;
}

void xylem_waitgroup_destroy(xylem_waitgroup_t* wg) {
    if (!wg) {
        return;
    }
    free(wg);
}

void xylem_waitgroup_add(xylem_waitgroup_t* wg, size_t delta) {
    atomic_fetch_add(&wg->cnt, delta);
}

void xylem_waitgroup_done(xylem_waitgroup_t* wg) {
    size_t prev = atomic_fetch_sub(&wg->cnt, 1);
    if (prev == 1) {
        /* cnt reached zero -- wake the waiting coroutine via loop post. */
        loop_post(runtime_get_loop(), _waitgroup_wakeup_cb, wg);
    }
}

void xylem_waitgroup_wait(xylem_waitgroup_t* wg) {
    if (atomic_load(&wg->cnt) == 0) {
        return;
    }
    wg->wait_coro = mco_running();
    mco_yield(mco_running());
}
