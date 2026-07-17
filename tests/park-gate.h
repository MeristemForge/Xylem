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
#include <threads.h>

typedef struct {
    mtx_t lock;
    cnd_t cond;
    bool  entered;
    bool  released;
} park_gate_t;

static int park_gate_init(park_gate_t* gate) {
    gate->entered  = false;
    gate->released = false;
    if (mtx_init(&gate->lock, mtx_plain) != thrd_success) {
        return -1;
    }
    if (cnd_init(&gate->cond) != thrd_success) {
        mtx_destroy(&gate->lock);
        return -1;
    }
    return 0;
}

static void park_gate_deinit(park_gate_t* gate) {
    cnd_destroy(&gate->cond);
    mtx_destroy(&gate->lock);
}

static void park_gate_hook(void* arg) {
    park_gate_t* gate = (park_gate_t*)arg;
    mtx_lock(&gate->lock);
    gate->entered = true;
    cnd_broadcast(&gate->cond);
    while (!gate->released) {
        cnd_wait(&gate->cond, &gate->lock);
    }
    mtx_unlock(&gate->lock);
}

static void park_gate_wait(park_gate_t* gate) {
    mtx_lock(&gate->lock);
    while (!gate->entered) {
        cnd_wait(&gate->cond, &gate->lock);
    }
    mtx_unlock(&gate->lock);
}

static void park_gate_release(park_gate_t* gate) {
    mtx_lock(&gate->lock);
    gate->released = true;
    cnd_broadcast(&gate->cond);
    mtx_unlock(&gate->lock);
}
