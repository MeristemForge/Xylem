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
#include <stdatomic.h>
#include <stddef.h>

#define mpsc_entry(x, t, m) ((t*)((char*)(x) - offsetof(t, m)))

typedef struct mpsc_node_s mpsc_node_t;

struct mpsc_node_s {
    _Atomic(mpsc_node_t*) next;
};

typedef struct mpsc_s {
    _Atomic(mpsc_node_t*) tail;
    mpsc_node_t*          head;
    mpsc_node_t           sentinel;
} mpsc_t;

/**
 * @brief Initialize an MPSC queue.
 *
 * @param q  Pointer to the queue structure to initialize.
 */
extern void mpsc_init(mpsc_t* q);

/**
 * @brief Push a node onto the queue (multi-producer safe).
 *
 * @param q     Pointer to the queue.
 * @param node  Pointer to the intrusive node to push.
 */
extern void mpsc_push(mpsc_t* q, mpsc_node_t* node);

/**
 * @brief Pop a node from the queue (single-consumer only).
 *
 * @param q  Pointer to the queue.
 *
 * @return Pointer to the popped node, or NULL if the queue is empty
 *         or temporarily inconsistent.
 */
extern mpsc_node_t* mpsc_pop(mpsc_t* q);

/**
 * @brief Check whether the queue appears empty.
 *
 * This is a best-effort check. A concurrent push may be in progress.
 *
 * @param q  Pointer to the queue.
 *
 * @return true if the queue appears empty, false otherwise.
 */
extern bool mpsc_empty(mpsc_t* q);
