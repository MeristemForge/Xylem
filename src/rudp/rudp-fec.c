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

#include "rudp-fec.h"

#include "xylem/xylem-fec.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Maximum concurrent shard groups kept before discarding old ones. */
#define RUDP_FEC_MAX_GROUPS  3

struct rudp_fec_enc_s {
    xylem_fec_t* codec;
    int          data_shards;
    int          parity_shards;
    int          shard_size;     /* data_shards + parity_shards */
    int          mtu;            /* max shard size on wire */
    uint32_t     next_seqid;
    uint32_t     paws;           /* seqid wrap boundary */
    int          shard_count;    /* data shards collected so far */
    int          max_payload;    /* max payload length in current group */
    uint8_t*     buf;            /* shard_size * mtu contiguous buffer */
    uint8_t**    shard_ptrs;     /* pointers into buf for RS encode */
    uint16_t*    payload_sizes;  /* actual payload size per data shard */
};

typedef struct {
    uint32_t  group_id;
    int       count;             /* shards received so far */
    int       max_payload;       /* max payload length seen */
    bool      present[RUDP_FEC_MAX_SHARDS];
    uint16_t  payload_sizes[RUDP_FEC_MAX_SHARDS];
} _fec_group_t;

struct rudp_fec_dec_s {
    xylem_fec_t* codec;
    int          data_shards;
    int          parity_shards;
    int          shard_size;     /* data_shards + parity_shards */
    int          mtu;            /* max shard size on wire */
    uint32_t     newest_group;   /* highest group_id seen */
    bool         has_newest;     /* whether newest_group is valid */
    uint8_t*     buf;            /* MAX_GROUPS * shard_size * mtu */
    uint8_t**    shard_ptrs;     /* scratch for RS reconstruct */
    uint8_t*     marks;          /* scratch for RS reconstruct */
    _fec_group_t groups[RUDP_FEC_MAX_GROUPS];
};

static uint32_t _fec_read_u32_le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t _fec_read_u16_le(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void _fec_write_u32_le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void _fec_write_u16_le(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

rudp_fec_enc_t* rudp_fec_enc_create(int data_shards, int parity_shards,
                                    int mtu) {
    if (data_shards <= 0 || parity_shards <= 0 || mtu <= 0) {
        return NULL;
    }
    int total = data_shards + parity_shards;
    if (total > RUDP_FEC_MAX_SHARDS) {
        return NULL;
    }

    xylem_fec_t* codec = xylem_fec_create(data_shards, parity_shards);
    if (!codec) {
        return NULL;
    }

    rudp_fec_enc_t* enc = calloc(1, sizeof(*enc));
    if (!enc) {
        xylem_fec_destroy(codec);
        return NULL;
    }

    enc->buf = calloc((size_t)total, (size_t)mtu);
    enc->shard_ptrs = calloc((size_t)total, sizeof(uint8_t*));
    enc->payload_sizes = calloc((size_t)data_shards, sizeof(uint16_t));

    if (!enc->buf || !enc->shard_ptrs || !enc->payload_sizes) {
        free(enc->buf);
        free(enc->shard_ptrs);
        free(enc->payload_sizes);
        free(enc);
        xylem_fec_destroy(codec);
        return NULL;
    }

    for (int i = 0; i < total; i++) {
        enc->shard_ptrs[i] = enc->buf + (size_t)i * (size_t)mtu;
    }

    enc->codec         = codec;
    enc->data_shards   = data_shards;
    enc->parity_shards = parity_shards;
    enc->shard_size    = total;
    enc->mtu           = mtu;
    enc->paws          = 0xFFFFFFFF / (uint32_t)total * (uint32_t)total;
    return enc;
}

void rudp_fec_enc_destroy(rudp_fec_enc_t* enc) {
    if (!enc) {
        return;
    }
    xylem_fec_destroy(enc->codec);
    free(enc->buf);
    free(enc->shard_ptrs);
    free(enc->payload_sizes);
    free(enc);
}

int rudp_fec_enc_feed_size(rudp_fec_enc_t* enc) {
    return 1 + enc->parity_shards;
}

int rudp_fec_enc_feed(rudp_fec_enc_t* enc,
                      const void* src, size_t slen,
                      rudp_fec_buf_t* dst, int dlen) {
    if (dlen < 1 + enc->parity_shards) {
        return -1;
    }

    int dst_count = 0;
    int idx = enc->shard_count;
    uint8_t* shard = enc->shard_ptrs[idx];

    _fec_write_u32_le(shard, enc->next_seqid);
    _fec_write_u16_le(shard + 4, RUDP_FEC_TYPE_DATA);
    _fec_write_u16_le(shard + 6, (uint16_t)slen);

    memcpy(shard + RUDP_FEC_HEADER_SIZE, src, slen);

    enc->payload_sizes[idx] = (uint16_t)slen;
    enc->next_seqid = (enc->next_seqid + 1) % enc->paws;

    int total_shard_len = RUDP_FEC_HEADER_SIZE + (int)slen;
    if (total_shard_len > enc->max_payload) {
        enc->max_payload = total_shard_len;
    }

    /* Output this data shard. */
    dst[dst_count].data = shard;
    dst[dst_count].len  = (size_t)total_shard_len;
    dst_count++;

    enc->shard_count++;

    /* Generate parity when we have enough data shards. */
    if (enc->shard_count == enc->data_shards) {
        int encode_len = enc->max_payload - RUDP_FEC_HEADER_SIZE;
        if (encode_len <= 0) {
            encode_len = 1;
        }

        /**
         * Pad shorter data shards with zeros so all payloads (after
         * the FEC header) are equal-sized for Reed-Solomon encoding.
         */
        for (int i = 0; i < enc->data_shards; i++) {
            int actual = (int)enc->payload_sizes[i];
            if (actual < encode_len) {
                memset(enc->shard_ptrs[i] + RUDP_FEC_HEADER_SIZE + actual,
                       0, (size_t)(encode_len - actual));
            }
        }

        /* Build pointer arrays for the payload region only (skip header). */
        uint8_t* data_ptrs[RUDP_FEC_MAX_SHARDS];
        uint8_t* parity_ptrs[RUDP_FEC_MAX_SHARDS];
        for (int i = 0; i < enc->data_shards; i++) {
            data_ptrs[i] = enc->shard_ptrs[i] + RUDP_FEC_HEADER_SIZE;
        }
        for (int i = 0; i < enc->parity_shards; i++) {
            parity_ptrs[i] =
                enc->shard_ptrs[enc->data_shards + i] + RUDP_FEC_HEADER_SIZE;
        }

        if (xylem_fec_encode(enc->codec, data_ptrs, parity_ptrs,
                             (size_t)encode_len) == 0) {
            /* Write FEC headers for parity shards and output them. */
            for (int i = 0; i < enc->parity_shards; i++) {
                uint8_t* ps = enc->shard_ptrs[enc->data_shards + i];
                _fec_write_u32_le(ps, enc->next_seqid);
                _fec_write_u16_le(ps + 4, RUDP_FEC_TYPE_PARITY);
                _fec_write_u16_le(ps + 6, (uint16_t)encode_len);
                enc->next_seqid = (enc->next_seqid + 1) % enc->paws;
                dst[dst_count].data = ps;
                dst[dst_count].len  =
                    (size_t)(RUDP_FEC_HEADER_SIZE + encode_len);
                dst_count++;
            }
        } else {
            /* Encoding failed; advance seqid to keep monotonic. */
            enc->next_seqid = (enc->next_seqid +
                               (uint32_t)enc->parity_shards) % enc->paws;
        }

        enc->shard_count = 0;
        enc->max_payload = 0;
    }

    return dst_count;
}

rudp_fec_dec_t* rudp_fec_dec_create(int data_shards, int parity_shards,
                                    int mtu) {
    if (data_shards <= 0 || parity_shards <= 0 || mtu <= 0) {
        return NULL;
    }
    int total = data_shards + parity_shards;
    if (total > RUDP_FEC_MAX_SHARDS) {
        return NULL;
    }

    xylem_fec_t* codec = xylem_fec_create(data_shards, parity_shards);
    if (!codec) {
        return NULL;
    }

    rudp_fec_dec_t* dec = calloc(1, sizeof(*dec));
    if (!dec) {
        xylem_fec_destroy(codec);
        return NULL;
    }

    size_t buf_size = (size_t)RUDP_FEC_MAX_GROUPS * (size_t)total *
                      (size_t)mtu;
    dec->buf = calloc(1, buf_size);
    dec->shard_ptrs = calloc((size_t)total, sizeof(uint8_t*));
    dec->marks = calloc((size_t)total, sizeof(uint8_t));

    if (!dec->buf || !dec->shard_ptrs || !dec->marks) {
        free(dec->buf);
        free(dec->shard_ptrs);
        free(dec->marks);
        free(dec);
        xylem_fec_destroy(codec);
        return NULL;
    }

    dec->codec         = codec;
    dec->data_shards   = data_shards;
    dec->parity_shards = parity_shards;
    dec->shard_size    = total;
    dec->mtu           = mtu;
    return dec;
}

void rudp_fec_dec_destroy(rudp_fec_dec_t* dec) {
    if (!dec) {
        return;
    }
    xylem_fec_destroy(dec->codec);
    free(dec->buf);
    free(dec->shard_ptrs);
    free(dec->marks);
    free(dec);
}

/* Get the buffer for a specific group slot and shard index. */
static uint8_t* _fec_dec_shard_buf(rudp_fec_dec_t* dec,
                                   int group_slot, int shard_idx) {
    size_t offset = ((size_t)group_slot * (size_t)dec->shard_size +
                     (size_t)shard_idx) * (size_t)dec->mtu;
    return dec->buf + offset;
}

/* Find or allocate a group slot. Returns slot index or -1. */
static int _fec_dec_find_group(rudp_fec_dec_t* dec, uint32_t group_id) {
    /* Check existing groups. */
    for (int i = 0; i < RUDP_FEC_MAX_GROUPS; i++) {
        if (dec->groups[i].count > 0 &&
            dec->groups[i].group_id == group_id) {
            return i;
        }
    }

    /* Find an empty slot. */
    for (int i = 0; i < RUDP_FEC_MAX_GROUPS; i++) {
        if (dec->groups[i].count == 0) {
            dec->groups[i].group_id = group_id;
            return i;
        }
    }

    /* Evict the oldest group (lowest group_id). */
    int oldest = 0;
    for (int i = 1; i < RUDP_FEC_MAX_GROUPS; i++) {
        /* Use signed diff for wrap-around comparison. */
        int32_t diff = (int32_t)(dec->groups[i].group_id -
                                 dec->groups[oldest].group_id);
        if (diff < 0) {
            oldest = i;
        }
    }
    memset(&dec->groups[oldest], 0, sizeof(dec->groups[oldest]));
    dec->groups[oldest].group_id = group_id;
    return oldest;
}

/* Discard groups that are too old relative to newest_group. */
static void _fec_dec_discard_old(rudp_fec_dec_t* dec) {
    if (!dec->has_newest) {
        return;
    }
    for (int i = 0; i < RUDP_FEC_MAX_GROUPS; i++) {
        if (dec->groups[i].count == 0) {
            continue;
        }
        int32_t age = (int32_t)(dec->newest_group -
                                dec->groups[i].group_id);
        if (age > RUDP_FEC_MAX_GROUPS) {
            memset(&dec->groups[i], 0, sizeof(dec->groups[i]));
        }
    }
}

int rudp_fec_dec_feed_size(rudp_fec_dec_t* dec) {
    return dec->data_shards;
}

int rudp_fec_dec_feed(rudp_fec_dec_t* dec,
                      const void* src, size_t slen,
                      rudp_fec_buf_t* dst, int dlen) {
    if (dlen < dec->data_shards) {
        return -1;
    }

    int dst_count = 0;

    if (slen < RUDP_FEC_HEADER_SIZE) {
        return -1;
    }

    const uint8_t* p = (const uint8_t*)src;
    uint32_t seqid = _fec_read_u32_le(p);
    uint16_t type  = _fec_read_u16_le(p + 4);
    uint16_t size  = _fec_read_u16_le(p + 6);

    if (type != RUDP_FEC_TYPE_DATA && type != RUDP_FEC_TYPE_PARITY) {
        return -1;
    }

    uint32_t group_id = seqid / (uint32_t)dec->shard_size;
    int slot_idx = (int)(seqid % (uint32_t)dec->shard_size);

    /* Update newest group for age-based eviction. */
    if (!dec->has_newest) {
        dec->newest_group = group_id;
        dec->has_newest = true;
    } else {
        int32_t diff = (int32_t)(group_id - dec->newest_group);
        if (diff > 0) {
            dec->newest_group = group_id;
        }
    }

    int gi = _fec_dec_find_group(dec, group_id);
    if (gi < 0) {
        return -1;
    }

    _fec_group_t* grp = &dec->groups[gi];

    /* Ignore duplicate shards. */
    if (grp->present[slot_idx]) {
        if (type == RUDP_FEC_TYPE_DATA) {
            dst[dst_count].data = (uint8_t*)p + RUDP_FEC_HEADER_SIZE;
            dst[dst_count].len  = size;
            dst_count++;
        }
        return dst_count;
    }

    /* Store the shard payload into the group buffer. */
    size_t payload_len = slen - RUDP_FEC_HEADER_SIZE;
    uint8_t* slot_buf = _fec_dec_shard_buf(dec, gi, slot_idx);
    memcpy(slot_buf, p + RUDP_FEC_HEADER_SIZE, payload_len);

    grp->present[slot_idx] = true;
    grp->payload_sizes[slot_idx] = size;
    grp->count++;

    if ((int)payload_len > grp->max_payload) {
        grp->max_payload = (int)payload_len;
    }

    /* Output the KCP payload for data shards. */
    if (type == RUDP_FEC_TYPE_DATA) {
        dst[dst_count].data = slot_buf;
        dst[dst_count].len  = size;
        dst_count++;
    }

    /* Try recovery when we have enough shards. */
    if (grp->count >= dec->data_shards) {
        /* Count how many data shards are missing. */
        int missing_data = 0;
        for (int i = 0; i < dec->data_shards; i++) {
            if (!grp->present[i]) {
                missing_data++;
            }
        }

        if (missing_data > 0) {
            int rs_len = grp->max_payload;
            if (rs_len <= 0) {
                rs_len = 1;
            }

            /* Build shard pointer array and marks for RS reconstruct. */
            for (int i = 0; i < dec->shard_size; i++) {
                dec->shard_ptrs[i] = _fec_dec_shard_buf(dec, gi, i);
                if (grp->present[i]) {
                    dec->marks[i] = 0;
                    /* Pad shorter shards to rs_len. */
                    int actual = (int)(i < dec->data_shards
                                           ? grp->payload_sizes[i]
                                           : (uint16_t)rs_len);
                    if (actual < rs_len) {
                        memset(dec->shard_ptrs[i] + actual, 0,
                               (size_t)(rs_len - actual));
                    }
                } else {
                    dec->marks[i] = 1;
                    memset(dec->shard_ptrs[i], 0, (size_t)rs_len);
                }
            }

            if (xylem_fec_reconstruct(dec->codec, dec->shard_ptrs,
                                      dec->marks, (size_t)rs_len) == 0) {
                for (int i = 0; i < dec->data_shards; i++) {
                    if (!grp->present[i]) {
                        dst[dst_count].data = dec->shard_ptrs[i];
                        dst[dst_count].len  = (size_t)rs_len;
                        dst_count++;
                    }
                }
            }
        }

        /* Group is consumed; clear it. */
        memset(grp, 0, sizeof(*grp));
    }

    _fec_dec_discard_old(dec);
    return dst_count;
}
