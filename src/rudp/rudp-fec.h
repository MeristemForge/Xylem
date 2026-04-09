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

#include <stddef.h>
#include <stdint.h>

/* FEC header size in bytes, needed by RUDP to adjust KCP MTU. */
#define RUDP_FEC_HEADER_SIZE 8

/* FEC packet type markers, needed by RUDP server read dispatch. */
#define RUDP_FEC_TYPE_DATA   0xF1
#define RUDP_FEC_TYPE_PARITY 0xF2

/* Maximum total shards (data + parity) per FEC group. RS GF(2^8) limit. */
#define RUDP_FEC_MAX_SHARDS  255

typedef struct rudp_fec_enc_s rudp_fec_enc_t;
typedef struct rudp_fec_dec_s rudp_fec_dec_t;

/**
 * Generic data/length pair used as output by encoder and decoder.
 */
typedef struct rudp_fec_buf_s {
    uint8_t* data;
    size_t   len;
} rudp_fec_buf_t;

/**
 * @brief Create a FEC encoder.
 *
 * @param data_shards    Number of data shards per group (1..254).
 * @param parity_shards  Number of parity shards per group (1..255-data_shards).
 * @param mtu            Max UDP payload size (FEC header + KCP packet).
 *
 * @return Encoder handle, or NULL on invalid parameters or allocation failure.
 */
extern rudp_fec_enc_t* rudp_fec_enc_create(int data_shards,
                                           int parity_shards,
                                           int mtu);

/**
 * @brief Destroy a FEC encoder and free all associated memory.
 *
 * @param enc  Encoder handle. NULL is safely ignored.
 */
extern void rudp_fec_enc_destroy(rudp_fec_enc_t* enc);

/**
 * @brief Feed one KCP output packet into the encoder.
 *
 * The encoder prepends a FEC header and caches the shard. On every
 * call at least one data shard is produced. When data_shards packets
 * have been collected, parity shards are appended.
 *
 * @param enc   Encoder handle.
 * @param src   KCP packet data.
 * @param slen  KCP packet length in bytes.
 * @param dst   Output array, caller provides space for
 *              (1 + parity_shards) entries.
 *
 * @return Number of shards written to dst[].
 */
extern int rudp_fec_enc_feed(rudp_fec_enc_t* enc,
                             const void* src, size_t slen,
                             rudp_fec_buf_t* dst);

/**
 * @brief Create a FEC decoder.
 *
 * @param data_shards    Number of data shards per group (1..254).
 * @param parity_shards  Number of parity shards per group (1..255-data_shards).
 * @param mtu            Max UDP payload size (FEC header + KCP packet).
 *
 * @return Decoder handle, or NULL on invalid parameters or allocation failure.
 */
extern rudp_fec_dec_t* rudp_fec_dec_create(int data_shards,
                                           int parity_shards,
                                           int mtu);

/**
 * @brief Destroy a FEC decoder and free all associated memory.
 *
 * @param dec  Decoder handle. NULL is safely ignored.
 */
extern void rudp_fec_dec_destroy(rudp_fec_dec_t* dec);

/**
 * @brief Feed one received FEC shard into the decoder.
 *
 * Strips the FEC header, collects shards by group, and attempts
 * Reed-Solomon recovery when enough shards arrive. All resulting
 * KCP packets (both direct and recovered) are returned in dst[].
 *
 * @param dec   Decoder handle.
 * @param src   FEC shard (8-byte FEC header + KCP payload).
 * @param slen  Length of the FEC shard in bytes.
 * @param dst   Output array, caller provides space for
 *              data_shards entries.
 *
 * @return Number of KCP packets written to dst[] (>= 0), or -1 if
 *         the shard is invalid.
 */
extern int rudp_fec_dec_feed(rudp_fec_dec_t* dec,
                             const void* src, size_t slen,
                             rudp_fec_buf_t* dst);
