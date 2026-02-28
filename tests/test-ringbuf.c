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

/* Create with invalid params returns NULL. */
static void test_create_invalid(void) {
    ASSERT(xylem_ringbuf_create(0, 16) == NULL);   /* esize=0 */
    ASSERT(xylem_ringbuf_create(8, 4) == NULL);     /* bufsize < esize */
    ASSERT(xylem_ringbuf_create(1, 0) == NULL);     /* bufsize=0 */
}

/* Create and destroy, check initial state. */
static void test_create_destroy(void) {
    xylem_ringbuf_t* ring = xylem_ringbuf_create(1, 16);
    ASSERT(ring != NULL);
    ASSERT(xylem_ringbuf_empty(ring));
    ASSERT(!xylem_ringbuf_full(ring));
    ASSERT(xylem_ringbuf_len(ring) == 0);
    ASSERT(xylem_ringbuf_cap(ring) == 16);
    ASSERT(xylem_ringbuf_avail(ring) == 16);
    xylem_ringbuf_destroy(ring);
}

/* Capacity is rounded down to power of two. */
static void test_capacity_power_of_two(void) {
    /* 10 bytes / 1-byte entries -> rounded to 8 */
    xylem_ringbuf_t* ring = xylem_ringbuf_create(1, 10);
    ASSERT(ring != NULL);
    ASSERT(xylem_ringbuf_cap(ring) == 8);
    xylem_ringbuf_destroy(ring);

    /* 32 bytes / 4-byte entries = 8 entries (already pow2) */
    ring = xylem_ringbuf_create(4, 32);
    ASSERT(ring != NULL);
    ASSERT(xylem_ringbuf_cap(ring) == 8);
    xylem_ringbuf_destroy(ring);

    /* 20 bytes / 4-byte entries = 5 -> rounded to 4 */
    ring = xylem_ringbuf_create(4, 20);
    ASSERT(ring != NULL);
    ASSERT(xylem_ringbuf_cap(ring) == 4);
    xylem_ringbuf_destroy(ring);
}

/* Basic write and read of single bytes. */
static void test_write_read_bytes(void) {
    xylem_ringbuf_t* ring = xylem_ringbuf_create(1, 8);
    ASSERT(ring != NULL);

    uint8_t wdata[] = {1, 2, 3, 4, 5};
    size_t  written = xylem_ringbuf_write(ring, wdata, 5);
    ASSERT(written == 5);
    ASSERT(xylem_ringbuf_len(ring) == 5);
    ASSERT(xylem_ringbuf_avail(ring) == 3);

    uint8_t rdata[8] = {0};
    size_t  nread = xylem_ringbuf_read(ring, rdata, 5);
    ASSERT(nread == 5);
    ASSERT(memcmp(rdata, wdata, 5) == 0);
    ASSERT(xylem_ringbuf_empty(ring));

    xylem_ringbuf_destroy(ring);
}

/* Write fills buffer completely. */
static void test_write_until_full(void) {
    xylem_ringbuf_t* ring = xylem_ringbuf_create(1, 4);
    ASSERT(ring != NULL);
    ASSERT(xylem_ringbuf_cap(ring) == 4);

    uint8_t data[] = {10, 20, 30, 40};
    size_t  written = xylem_ringbuf_write(ring, data, 4);
    ASSERT(written == 4);
    ASSERT(xylem_ringbuf_full(ring));
    ASSERT(xylem_ringbuf_avail(ring) == 0);

    /* Write when full returns 0. */
    uint8_t extra = 50;
    written = xylem_ringbuf_write(ring, &extra, 1);
    ASSERT(written == 0);

    uint8_t out[4];
    size_t  nread = xylem_ringbuf_read(ring, out, 4);
    ASSERT(nread == 4);
    ASSERT(memcmp(out, data, 4) == 0);

    xylem_ringbuf_destroy(ring);
}

/* Read from empty returns 0. */
static void test_read_empty(void) {
    xylem_ringbuf_t* ring = xylem_ringbuf_create(1, 8);
    ASSERT(ring != NULL);

    uint8_t buf[4];
    size_t  nread = xylem_ringbuf_read(ring, buf, 4);
    ASSERT(nread == 0);

    xylem_ringbuf_destroy(ring);
}

/* Partial read: request more than available. */
static void test_partial_read(void) {
    xylem_ringbuf_t* ring = xylem_ringbuf_create(1, 8);
    ASSERT(ring != NULL);

    uint8_t wdata[] = {1, 2, 3};
    xylem_ringbuf_write(ring, wdata, 3);

    uint8_t rdata[8] = {0};
    size_t  nread = xylem_ringbuf_read(ring, rdata, 10);
    ASSERT(nread == 3);
    ASSERT(memcmp(rdata, wdata, 3) == 0);

    xylem_ringbuf_destroy(ring);
}

/* Wrap-around: write, read some, write more to wrap. */
static void test_wraparound(void) {
    xylem_ringbuf_t* ring = xylem_ringbuf_create(1, 4);
    ASSERT(ring != NULL);
    ASSERT(xylem_ringbuf_cap(ring) == 4);

    /* Fill 3 of 4 slots */
    uint8_t w1[] = {1, 2, 3};
    xylem_ringbuf_write(ring, w1, 3);

    /* Read 2 -> frees slots at front */
    uint8_t r1[2];
    xylem_ringbuf_read(ring, r1, 2);
    ASSERT(r1[0] == 1 && r1[1] == 2);

    /* Write 3 more -> wraps around */
    uint8_t w2[] = {4, 5, 6};
    size_t  written = xylem_ringbuf_write(ring, w2, 3);
    ASSERT(written == 3);
    ASSERT(xylem_ringbuf_len(ring) == 4);
    ASSERT(xylem_ringbuf_full(ring));

    /* Read all 4: should be 3, 4, 5, 6 */
    uint8_t r2[4];
    size_t  nread = xylem_ringbuf_read(ring, r2, 4);
    ASSERT(nread == 4);
    ASSERT(r2[0] == 3 && r2[1] == 4 && r2[2] == 5 && r2[3] == 6);

    xylem_ringbuf_destroy(ring);
}

/* Multi-byte entries (e.g., uint32_t). */
static void test_multibyte_entries(void) {
    xylem_ringbuf_t* ring = xylem_ringbuf_create(sizeof(uint32_t), 32);
    ASSERT(ring != NULL);
    /* 32 / 4 = 8 entries */
    ASSERT(xylem_ringbuf_cap(ring) == 8);

    uint32_t wdata[] = {100, 200, 300, 400, 500};
    size_t   written = xylem_ringbuf_write(ring, wdata, 5);
    ASSERT(written == 5);
    ASSERT(xylem_ringbuf_len(ring) == 5);

    uint32_t rdata[5] = {0};
    size_t   nread = xylem_ringbuf_read(ring, rdata, 5);
    ASSERT(nread == 5);
    for (int i = 0; i < 5; i++) {
        ASSERT(rdata[i] == wdata[i]);
    }

    xylem_ringbuf_destroy(ring);
}

/* Multibyte wrap-around. */
static void test_multibyte_wraparound(void) {
    xylem_ringbuf_t* ring = xylem_ringbuf_create(sizeof(uint32_t), 16);
    ASSERT(ring != NULL);
    /* 16 / 4 = 4 entries */
    ASSERT(xylem_ringbuf_cap(ring) == 4);

    uint32_t w1[] = {10, 20, 30};
    xylem_ringbuf_write(ring, w1, 3);

    uint32_t r1[2];
    xylem_ringbuf_read(ring, r1, 2);
    ASSERT(r1[0] == 10 && r1[1] == 20);

    uint32_t w2[] = {40, 50, 60};
    size_t   written = xylem_ringbuf_write(ring, w2, 3);
    ASSERT(written == 3);
    ASSERT(xylem_ringbuf_full(ring));

    uint32_t r2[4];
    size_t   nread = xylem_ringbuf_read(ring, r2, 4);
    ASSERT(nread == 4);
    ASSERT(r2[0] == 30 && r2[1] == 40 && r2[2] == 50 && r2[3] == 60);

    xylem_ringbuf_destroy(ring);
}

/* Repeated write/read cycles. */
static void test_repeated_cycles(void) {
    xylem_ringbuf_t* ring = xylem_ringbuf_create(1, 4);
    ASSERT(ring != NULL);

    for (int cycle = 0; cycle < 100; cycle++) {
        uint8_t w = (uint8_t)(cycle & 0xFF);
        size_t  written = xylem_ringbuf_write(ring, &w, 1);
        ASSERT(written == 1);

        uint8_t r;
        size_t  nread = xylem_ringbuf_read(ring, &r, 1);
        ASSERT(nread == 1);
        ASSERT(r == w);
    }
    ASSERT(xylem_ringbuf_empty(ring));

    xylem_ringbuf_destroy(ring);
}

int main(void) {
    test_create_invalid();
    test_create_destroy();
    test_capacity_power_of_two();
    test_write_read_bytes();
    test_write_until_full();
    test_read_empty();
    test_partial_read();
    test_wraparound();
    test_multibyte_entries();
    test_multibyte_wraparound();
    test_repeated_cycles();
    return 0;
}
