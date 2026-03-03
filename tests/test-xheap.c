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
#include "assert.h"

static int _int_cmp(const void* a, const void* b) {
    int va = *(const int*)a;
    int vb = *(const int*)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static void test_init(void) {
    xylem_xheap_t heap;
    xylem_xheap_init(&heap, _int_cmp);
    ASSERT(xylem_xheap_empty(&heap));
    ASSERT(xylem_xheap_len(&heap) == 0);
    ASSERT(xylem_xheap_root(&heap) == NULL);
}

static void test_insert_dequeue(void) {
    xylem_xheap_t heap;
    xylem_xheap_init(&heap, _int_cmp);

    int vals[] = {30, 10, 20, 5, 25};
    for (int i = 0; i < 5; i++) {
        ASSERT(xylem_xheap_insert(&heap, &vals[i]) == 0);
    }

    ASSERT(xylem_xheap_len(&heap) == 5);

    /* Min-heap: should dequeue in ascending order */
    int expected[] = {5, 10, 20, 25, 30};
    for (int i = 0; i < 5; i++) {
        ASSERT(*(int*)xylem_xheap_root(&heap) == expected[i]);
        xylem_xheap_dequeue(&heap);
    }
    ASSERT(xylem_xheap_empty(&heap));
}

static void test_clear(void) {
    xylem_xheap_t heap;
    xylem_xheap_init(&heap, _int_cmp);

    int vals[100];
    for (int i = 0; i < 100; i++) {
        vals[i] = i;
        xylem_xheap_insert(&heap, &vals[i]);
    }
    ASSERT(xylem_xheap_len(&heap) == 100);

    xylem_xheap_clear(&heap);
    ASSERT(xylem_xheap_empty(&heap));
}

static void test_descending_insert(void) {
    xylem_xheap_t heap;
    xylem_xheap_init(&heap, _int_cmp);

    int vals[50];
    for (int i = 49; i >= 0; i--) {
        vals[i] = i;
        xylem_xheap_insert(&heap, &vals[i]);
    }

    for (int i = 0; i < 50; i++) {
        ASSERT(*(int*)xylem_xheap_root(&heap) == i);
        xylem_xheap_dequeue(&heap);
    }
    ASSERT(xylem_xheap_empty(&heap));
}

int main(void) {
    test_init();
    test_insert_dequeue();
    test_clear();
    test_descending_insert();
    return 0;
}
