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

#include "container/mpsc.h"

#include "platform/platform-cpu.h"

#include <stddef.h>

void mpsc_init(mpsc_t* q) {
    atomic_store(&q->sentinel.next, NULL);
    atomic_store(&q->head, &q->sentinel);
    atomic_store(&q->tail, &q->sentinel);
}

void mpsc_push(mpsc_t* q, mpsc_node_t* node) {
    atomic_store(&node->next, NULL);
    mpsc_node_t* prev =
        atomic_exchange(&q->tail, node);
    atomic_store(&prev->next, node);
}

static void _mpsc_wait_link(void) {
    platform_cpu_relax();
}

mpsc_node_t* mpsc_pop(mpsc_t* q) {
    for (;;) {
        mpsc_node_t* head =
            atomic_load(&q->head);
        mpsc_node_t* next =
            atomic_load(&head->next);

        /* Skip the sentinel node. */
        if (head == &q->sentinel) {
            if (next == NULL) {
                if (head
                    != atomic_load(&q->tail)) {
                    _mpsc_wait_link();
                    continue;
                }
                return NULL;
            }
            atomic_store(&q->head, next);
            head = next;
            next = atomic_load(&head->next);
        }

        if (next != NULL) {
            atomic_store(&q->head, next);
            return head;
        }

        /* Only one node left; check if it is the tail. */
        if (head != atomic_load(&q->tail)) {
            /* A push is in progress but next is not yet visible. */
            _mpsc_wait_link();
            continue;
        }

        /* Re-insert the sentinel so the queue is never truly empty. */
        mpsc_push(q, &q->sentinel);

        next = atomic_load(&head->next);
        if (next != NULL) {
            atomic_store(&q->head, next);
            return head;
        }

        _mpsc_wait_link();
    }
}

bool mpsc_can_pop(mpsc_t* q) {
    mpsc_node_t* head = atomic_load(&q->head);
    mpsc_node_t* next =
        atomic_load(&head->next);

    if (head == &q->sentinel && next == NULL) {
        return false;
    }
    if (head == &q->sentinel && next != NULL) {
        return true;
    }
    return true;
}
