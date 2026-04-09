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
#include "xylem/xylem-logger.h"

#include <stdlib.h>
#include <string.h>
#include "deprecated/c11-threads.h"
#include "fec/reedsolomon-c/rs.h"

struct xylem_fec_s {
    reed_solomon* rs;
    int           data_shards;
    int           parity_shards;
    uint8_t**     enc_shards;
};

static once_flag _fec_init_flag = ONCE_FLAG_INIT;

static void _fec_init_tables(void) {
    reed_solomon_init();
}

xylem_fec_t* xylem_fec_create(int data_shards, int parity_shards) {
    call_once(&_fec_init_flag, _fec_init_tables);

    if (data_shards <= 0 || parity_shards <= 0) {
        xylem_loge("fec create: invalid shards (data=%d parity=%d)",
                   data_shards, parity_shards);
        return NULL;
    }
    if (data_shards + parity_shards > 255) {
        xylem_loge("fec create: total shards %d exceeds 255",
                   data_shards + parity_shards);
        return NULL;
    }

    reed_solomon* rs = reed_solomon_new(data_shards, parity_shards);
    if (!rs) {
        return NULL;
    }

    xylem_fec_t* fec = calloc(1, sizeof(*fec));
    if (!fec) {
        reed_solomon_release(rs);
        return NULL;
    }

    int total = data_shards + parity_shards;
    fec->enc_shards = malloc((size_t)total * sizeof(uint8_t*));
    if (!fec->enc_shards) {
        reed_solomon_release(rs);
        free(fec);
        return NULL;
    }

    fec->rs            = rs;
    fec->data_shards   = data_shards;
    fec->parity_shards = parity_shards;

    xylem_logi("fec created data=%d parity=%d", data_shards, parity_shards);
    return fec;
}

void xylem_fec_destroy(xylem_fec_t* fec) {
    if (!fec) {
        return;
    }
    reed_solomon_release(fec->rs);
    free(fec->enc_shards);
    free(fec);
}

int xylem_fec_encode(xylem_fec_t* fec, uint8_t** data, uint8_t** parity,
                     size_t shard_size) {
    if (!fec || !data || !parity || shard_size == 0) {
        return -1;
    }

    int total = fec->data_shards + fec->parity_shards;
    uint8_t** shards = fec->enc_shards;

    memcpy(shards, data, (size_t)fec->data_shards * sizeof(uint8_t*));
    memcpy(shards + fec->data_shards, parity,
           (size_t)fec->parity_shards * sizeof(uint8_t*));

    int rc = reed_solomon_encode(fec->rs, shards, total, (int)shard_size);
    if (rc != 0) {
        xylem_logw("fec encode failed");
    }
    return rc == 0 ? 0 : -1;
}

int xylem_fec_reconstruct(xylem_fec_t* fec, uint8_t** shards, uint8_t* marks,
                          size_t shard_size) {
    if (!fec || !shards || !marks || shard_size == 0) {
        return -1;
    }

    int total = fec->data_shards + fec->parity_shards;
    int rc = reed_solomon_reconstruct(fec->rs, shards, marks, total,
                                      (int)shard_size);
    if (rc != 0) {
        xylem_logw("fec reconstruct failed");
    }
    return rc == 0 ? 0 : -1;
}
