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

#include "assert.h"
#include "xylem/xylem-fec.h"

#include <stdlib.h>
#include <string.h>

#define DATA_SHARDS   5
#define PARITY_SHARDS 2
#define SHARD_SIZE    64
#define TOTAL_SHARDS  (DATA_SHARDS + PARITY_SHARDS)

/* Create and destroy without error. */
static void test_create_destroy(void) {
    xylem_fec_t* fec = xylem_fec_create(DATA_SHARDS, PARITY_SHARDS);
    ASSERT(fec != NULL);
    xylem_fec_destroy(fec);
}

/* Destroy NULL is safe. */
static void test_destroy_null(void) {
    xylem_fec_destroy(NULL);
}

/* Invalid parameters return NULL. */
static void test_create_invalid(void) {
    ASSERT(xylem_fec_create(0, 2) == NULL);
    ASSERT(xylem_fec_create(2, 0) == NULL);
    ASSERT(xylem_fec_create(-1, 2) == NULL);
    ASSERT(xylem_fec_create(2, -1) == NULL);
    ASSERT(xylem_fec_create(254, 2) == NULL);  /* 256 > 255 */
}

/* Encode then reconstruct recovers lost data shards. */
static void test_encode_reconstruct_data(void) {
    xylem_fec_t* fec = xylem_fec_create(DATA_SHARDS, PARITY_SHARDS);
    ASSERT(fec != NULL);

    uint8_t buf[TOTAL_SHARDS][SHARD_SIZE];
    uint8_t* data[DATA_SHARDS];
    uint8_t* parity[PARITY_SHARDS];

    for (int i = 0; i < DATA_SHARDS; i++) {
        for (int j = 0; j < SHARD_SIZE; j++) {
            buf[i][j] = (uint8_t)((i * SHARD_SIZE + j) % 251);
        }
        data[i] = buf[i];
    }
    for (int i = 0; i < PARITY_SHARDS; i++) {
        parity[i] = buf[DATA_SHARDS + i];
    }

    ASSERT(xylem_fec_encode(fec, data, parity, SHARD_SIZE) == 0);

    /* Save original data[1] for verification. */
    uint8_t saved[SHARD_SIZE];
    memcpy(saved, buf[1], SHARD_SIZE);

    /* Simulate loss of data shard 1. */
    memset(buf[1], 0, SHARD_SIZE);

    uint8_t* shards[TOTAL_SHARDS];
    for (int i = 0; i < TOTAL_SHARDS; i++) {
        shards[i] = buf[i];
    }
    uint8_t marks[TOTAL_SHARDS] = {0, 1, 0, 0, 0, 0, 0};

    ASSERT(xylem_fec_reconstruct(fec, shards, marks, SHARD_SIZE) == 0);
    ASSERT(memcmp(buf[1], saved, SHARD_SIZE) == 0);

    xylem_fec_destroy(fec);
}

/* Encode then reconstruct with lost parity succeeds (data intact). */
static void test_encode_reconstruct_parity(void) {
    xylem_fec_t* fec = xylem_fec_create(DATA_SHARDS, PARITY_SHARDS);
    ASSERT(fec != NULL);

    uint8_t buf[TOTAL_SHARDS][SHARD_SIZE];
    uint8_t* data[DATA_SHARDS];
    uint8_t* parity[PARITY_SHARDS];

    for (int i = 0; i < DATA_SHARDS; i++) {
        for (int j = 0; j < SHARD_SIZE; j++) {
            buf[i][j] = (uint8_t)((i + j) % 199);
        }
        data[i] = buf[i];
    }
    for (int i = 0; i < PARITY_SHARDS; i++) {
        parity[i] = buf[DATA_SHARDS + i];
    }

    ASSERT(xylem_fec_encode(fec, data, parity, SHARD_SIZE) == 0);

    /* Save all data shards. */
    uint8_t saved[DATA_SHARDS][SHARD_SIZE];
    for (int i = 0; i < DATA_SHARDS; i++) {
        memcpy(saved[i], buf[i], SHARD_SIZE);
    }

    /* Simulate loss of parity shard 0. */
    memset(buf[DATA_SHARDS], 0, SHARD_SIZE);

    uint8_t* shards[TOTAL_SHARDS];
    for (int i = 0; i < TOTAL_SHARDS; i++) {
        shards[i] = buf[i];
    }
    uint8_t marks[TOTAL_SHARDS] = {0, 0, 0, 0, 0, 1, 0};

    /* Succeeds -- no data shards lost, nothing to recover. */
    ASSERT(xylem_fec_reconstruct(fec, shards, marks, SHARD_SIZE) == 0);

    /* Data shards remain intact. */
    for (int i = 0; i < DATA_SHARDS; i++) {
        ASSERT(memcmp(buf[i], saved[i], SHARD_SIZE) == 0);
    }

    xylem_fec_destroy(fec);
}

/* Lose maximum number of shards (parity_shards) and recover. */
static void test_max_loss(void) {
    xylem_fec_t* fec = xylem_fec_create(DATA_SHARDS, PARITY_SHARDS);
    ASSERT(fec != NULL);

    uint8_t buf[TOTAL_SHARDS][SHARD_SIZE];
    uint8_t* data[DATA_SHARDS];
    uint8_t* parity[PARITY_SHARDS];

    for (int i = 0; i < DATA_SHARDS; i++) {
        for (int j = 0; j < SHARD_SIZE; j++) {
            buf[i][j] = (uint8_t)((i * 7 + j * 3) % 241);
        }
        data[i] = buf[i];
    }
    for (int i = 0; i < PARITY_SHARDS; i++) {
        parity[i] = buf[DATA_SHARDS + i];
    }

    ASSERT(xylem_fec_encode(fec, data, parity, SHARD_SIZE) == 0);

    /* Save data[0] and data[2]. */
    uint8_t saved0[SHARD_SIZE];
    uint8_t saved2[SHARD_SIZE];
    memcpy(saved0, buf[0], SHARD_SIZE);
    memcpy(saved2, buf[2], SHARD_SIZE);

    /* Lose 2 data shards (the maximum). */
    memset(buf[0], 0, SHARD_SIZE);
    memset(buf[2], 0, SHARD_SIZE);

    uint8_t* shards[TOTAL_SHARDS];
    for (int i = 0; i < TOTAL_SHARDS; i++) {
        shards[i] = buf[i];
    }
    uint8_t marks[TOTAL_SHARDS] = {1, 0, 1, 0, 0, 0, 0};

    ASSERT(xylem_fec_reconstruct(fec, shards, marks, SHARD_SIZE) == 0);
    ASSERT(memcmp(buf[0], saved0, SHARD_SIZE) == 0);
    ASSERT(memcmp(buf[2], saved2, SHARD_SIZE) == 0);

    xylem_fec_destroy(fec);
}

/* Too many losses fails. */
static void test_too_many_losses(void) {
    xylem_fec_t* fec = xylem_fec_create(DATA_SHARDS, PARITY_SHARDS);
    ASSERT(fec != NULL);

    uint8_t buf[TOTAL_SHARDS][SHARD_SIZE];
    uint8_t* data[DATA_SHARDS];
    uint8_t* parity[PARITY_SHARDS];

    for (int i = 0; i < DATA_SHARDS; i++) {
        memset(buf[i], (uint8_t)i, SHARD_SIZE);
        data[i] = buf[i];
    }
    for (int i = 0; i < PARITY_SHARDS; i++) {
        parity[i] = buf[DATA_SHARDS + i];
    }

    ASSERT(xylem_fec_encode(fec, data, parity, SHARD_SIZE) == 0);

    /* Lose 3 shards -- exceeds parity_shards (2). */
    uint8_t* shards[TOTAL_SHARDS];
    for (int i = 0; i < TOTAL_SHARDS; i++) {
        shards[i] = buf[i];
    }
    uint8_t marks[TOTAL_SHARDS] = {1, 1, 1, 0, 0, 0, 0};

    ASSERT(xylem_fec_reconstruct(fec, shards, marks, SHARD_SIZE) == -1);

    xylem_fec_destroy(fec);
}

/* No losses -- reconstruct is a no-op success. */
static void test_no_loss(void) {
    xylem_fec_t* fec = xylem_fec_create(DATA_SHARDS, PARITY_SHARDS);
    ASSERT(fec != NULL);

    uint8_t buf[TOTAL_SHARDS][SHARD_SIZE];
    uint8_t* data[DATA_SHARDS];
    uint8_t* parity[PARITY_SHARDS];

    for (int i = 0; i < DATA_SHARDS; i++) {
        memset(buf[i], (uint8_t)(i + 10), SHARD_SIZE);
        data[i] = buf[i];
    }
    for (int i = 0; i < PARITY_SHARDS; i++) {
        parity[i] = buf[DATA_SHARDS + i];
    }

    ASSERT(xylem_fec_encode(fec, data, parity, SHARD_SIZE) == 0);

    uint8_t saved[TOTAL_SHARDS][SHARD_SIZE];
    memcpy(saved, buf, sizeof(buf));

    uint8_t* shards[TOTAL_SHARDS];
    for (int i = 0; i < TOTAL_SHARDS; i++) {
        shards[i] = buf[i];
    }
    uint8_t marks[TOTAL_SHARDS] = {0};

    ASSERT(xylem_fec_reconstruct(fec, shards, marks, SHARD_SIZE) == 0);
    ASSERT(memcmp(buf, saved, sizeof(buf)) == 0);

    xylem_fec_destroy(fec);
}

/* Minimal config: 1 data + 1 parity. */
static void test_minimal_shards(void) {
    xylem_fec_t* fec = xylem_fec_create(1, 1);
    ASSERT(fec != NULL);

    uint8_t d[SHARD_SIZE];
    uint8_t p[SHARD_SIZE];
    memset(d, 0xAB, SHARD_SIZE);

    uint8_t* data[1]   = {d};
    uint8_t* parity[1] = {p};

    ASSERT(xylem_fec_encode(fec, data, parity, SHARD_SIZE) == 0);

    uint8_t saved[SHARD_SIZE];
    memcpy(saved, d, SHARD_SIZE);
    memset(d, 0, SHARD_SIZE);

    uint8_t* shards[2] = {d, p};
    uint8_t marks[2]   = {1, 0};

    ASSERT(xylem_fec_reconstruct(fec, shards, marks, SHARD_SIZE) == 0);
    ASSERT(memcmp(d, saved, SHARD_SIZE) == 0);

    xylem_fec_destroy(fec);
}

int main(void) {
    test_create_destroy();
    test_destroy_null();
    test_create_invalid();
    test_encode_reconstruct_data();
    test_encode_reconstruct_parity();
    test_max_loss();
    test_too_many_losses();
    test_no_loss();
    test_minimal_shards();
    return 0;
}
