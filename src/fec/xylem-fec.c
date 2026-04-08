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

#include "xylem/xylem-fec.h"

#include <stdlib.h>
#include "fec/reedsolomon-c/rs.h"

struct xylem_fec_s {
    reed_solomon *rs;
    int           data_shards;
    int           parity_shards;
};

void xylem_fec_init(void) {
    reed_solomon_init();
}

xylem_fec_t *xylem_fec_create(int data_shards, int parity_shards) {
    if (data_shards <= 0 || parity_shards <= 0) {
        return NULL;
    }
    if (data_shards + parity_shards > 255) {
        return NULL;
    }

    reed_solomon *rs = reed_solomon_new(data_shards, parity_shards);
    if (!rs) {
        return NULL;
    }

    xylem_fec_t *fec = calloc(1, sizeof(*fec));
    if (!fec) {
        reed_solomon_release(rs);
        return NULL;
    }

    fec->rs            = rs;
    fec->data_shards   = data_shards;
    fec->parity_shards = parity_shards;
    return fec;
}

void xylem_fec_destroy(xylem_fec_t *fec) {
    if (!fec) {
        return;
    }
    reed_solomon_release(fec->rs);
    free(fec);
}

int xylem_fec_data_shards(const xylem_fec_t *fec) {
    return fec->data_shards;
}

int xylem_fec_parity_shards(const xylem_fec_t *fec) {
    return fec->parity_shards;
}

int xylem_fec_encode(xylem_fec_t *fec, uint8_t **shards, size_t shard_size) {
    if (!fec || !shards || shard_size == 0) {
        return -1;
    }

    int total = fec->data_shards + fec->parity_shards;
    int rc = reed_solomon_encode(fec->rs, shards, total, (int)shard_size);
    return rc == 0 ? 0 : -1;
}

int xylem_fec_reconstruct(xylem_fec_t *fec, uint8_t **shards, uint8_t *marks,
                          size_t shard_size) {
    if (!fec || !shards || !marks || shard_size == 0) {
        return -1;
    }

    int total = fec->data_shards + fec->parity_shards;
    int rc = reed_solomon_reconstruct(fec->rs, shards, marks, total,
                                      (int)shard_size);
    return rc == 0 ? 0 : -1;
}
