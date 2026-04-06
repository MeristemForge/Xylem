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

#include "xylem/xylem-hmac256.h"
#include "xylem/xylem-sha256.h"

#include <string.h>

#define HMAC_BLOCK_SIZE  64
#define HMAC_DIGEST_SIZE 32

void xylem_hmac256_compute(const uint8_t* key, size_t key_len,
                       const uint8_t* msg, size_t msg_len,
                       uint8_t out[32]) {
    uint8_t k[HMAC_BLOCK_SIZE] = {0};

    /* Keys longer than the block size are hashed first. */
    if (key_len > HMAC_BLOCK_SIZE) {
        xylem_sha256_t* h = xylem_sha256_create();
        xylem_sha256_update(h, key, key_len);
        xylem_sha256_final(h, k);
        xylem_sha256_destroy(h);
    } else {
        memcpy(k, key, key_len);
    }

    uint8_t ipad[HMAC_BLOCK_SIZE];
    uint8_t opad[HMAC_BLOCK_SIZE];
    for (size_t i = 0; i < HMAC_BLOCK_SIZE; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    /* inner = SHA256(ipad || msg) */
    xylem_sha256_t* ictx = xylem_sha256_create();
    xylem_sha256_update(ictx, ipad, HMAC_BLOCK_SIZE);
    xylem_sha256_update(ictx, msg, msg_len);
    uint8_t inner[HMAC_DIGEST_SIZE];
    xylem_sha256_final(ictx, inner);
    xylem_sha256_destroy(ictx);

    /* out = SHA256(opad || inner) */
    xylem_sha256_t* octx = xylem_sha256_create();
    xylem_sha256_update(octx, opad, HMAC_BLOCK_SIZE);
    xylem_sha256_update(octx, inner, HMAC_DIGEST_SIZE);
    xylem_sha256_final(octx, out);
    xylem_sha256_destroy(octx);
}
