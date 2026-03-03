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

#include "xylem.h"

void xylem_queue_init(xylem_queue_t* queue) {
    queue->head.prev = &queue->head;
    queue->head.next = &queue->head;
    queue->nelts = 0;
}

bool xylem_queue_empty(xylem_queue_t* queue) {
    return queue->nelts == 0;
}

size_t xylem_queue_len(xylem_queue_t* queue) {
    return queue->nelts;
}

void xylem_queue_enqueue(xylem_queue_t* queue, xylem_queue_node_t* node) {
    node->prev = queue->head.prev;
    node->next = &queue->head;
    queue->head.prev->next = node;
    queue->head.prev = node;
    queue->nelts++;
}

xylem_queue_node_t* xylem_queue_dequeue(xylem_queue_t* queue) {
    if (xylem_queue_empty(queue)) return NULL;
    xylem_queue_node_t* node = queue->head.next;
    node->next->prev = &queue->head;
    queue->head.next = node->next;
    node->prev = NULL;
    node->next = NULL;
    queue->nelts--;
    return node;
}

xylem_queue_node_t* xylem_queue_front(xylem_queue_t* queue) {
    if (xylem_queue_empty(queue)) return NULL;
    return queue->head.next;
}

xylem_queue_node_t* xylem_queue_back(xylem_queue_t* queue) {
    if (xylem_queue_empty(queue)) return NULL;
    return queue->head.prev;
}

void xylem_queue_swap(xylem_queue_t* queue1, xylem_queue_t* queue2) {
    if (queue1 == queue2) return;

    bool q1_empty = (queue1->nelts == 0);
    bool q2_empty = (queue2->nelts == 0);

    if (q1_empty && q2_empty) return;

    if (q1_empty) {
        queue1->head.next = queue2->head.next;
        queue1->head.prev = queue2->head.prev;
        queue1->head.next->prev = &queue1->head;
        queue1->head.prev->next = &queue1->head;
        queue2->head.next = &queue2->head;
        queue2->head.prev = &queue2->head;
    } else if (q2_empty) {
        queue2->head.next = queue1->head.next;
        queue2->head.prev = queue1->head.prev;
        queue2->head.next->prev = &queue2->head;
        queue2->head.prev->next = &queue2->head;
        queue1->head.next = &queue1->head;
        queue1->head.prev = &queue1->head;
    } else {
        xylem_list_node_t* tmp_next = queue1->head.next;
        xylem_list_node_t* tmp_prev = queue1->head.prev;

        queue1->head.next = queue2->head.next;
        queue1->head.prev = queue2->head.prev;
        queue2->head.next = tmp_next;
        queue2->head.prev = tmp_prev;

        queue1->head.next->prev = &queue1->head;
        queue1->head.prev->next = &queue1->head;
        queue2->head.next->prev = &queue2->head;
        queue2->head.prev->next = &queue2->head;
    }

    size_t tmp     = queue1->nelts;
    queue1->nelts  = queue2->nelts;
    queue2->nelts  = tmp;
}
