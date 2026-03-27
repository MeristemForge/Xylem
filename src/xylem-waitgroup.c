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

#include "xylem/xylem-waitgroup.h"

#include <stdlib.h>

#include "deprecated/c11-threads.h"

struct xylem_waitgroup_s {
    size_t cnt;
    mtx_t  mtx;
    cnd_t  cnd;
};

xylem_waitgroup_t* xylem_waitgroup_create(void) {
    xylem_waitgroup_t* waitgroup = malloc(sizeof(xylem_waitgroup_t));
    if (!waitgroup) {
        return NULL;
    }
    waitgroup->cnt = 0;
    if (mtx_init(&waitgroup->mtx, mtx_plain) != thrd_success) {
        free(waitgroup);
        return NULL;
    }
    if (cnd_init(&waitgroup->cnd) != thrd_success) {
        mtx_destroy(&waitgroup->mtx);
        free(waitgroup);
        return NULL;
    }
    return waitgroup;
}

void xylem_waitgroup_destroy(xylem_waitgroup_t* waitgroup) {
    mtx_destroy(&waitgroup->mtx);
    cnd_destroy(&waitgroup->cnd);
    waitgroup->cnt = 0;

    free(waitgroup);
}

void xylem_waitgroup_add(xylem_waitgroup_t* waitgroup, size_t delta) {
    mtx_lock(&waitgroup->mtx);
    waitgroup->cnt += delta;
    mtx_unlock(&waitgroup->mtx);
}

void xylem_waitgroup_done(xylem_waitgroup_t* waitgroup) {
    mtx_lock(&waitgroup->mtx);
    if (waitgroup->cnt == 0) {
        mtx_unlock(&waitgroup->mtx);
        return;
    }
    waitgroup->cnt--;
    if (waitgroup->cnt == 0) {
        cnd_broadcast(&waitgroup->cnd);
    }
    mtx_unlock(&waitgroup->mtx);
}

void xylem_waitgroup_wait(xylem_waitgroup_t* waitgroup) {
    mtx_lock(&waitgroup->mtx);
    while (waitgroup->cnt) {
        cnd_wait(&waitgroup->cnd, &waitgroup->mtx);
    }
    mtx_unlock(&waitgroup->mtx);
}
