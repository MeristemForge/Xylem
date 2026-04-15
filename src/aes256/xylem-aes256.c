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

#include "xylem/xylem-aes256.h"

#include "platform/platform-info.h"

#include "aes256/tiny-AES-c/aes.h"

#include <stdlib.h>
#include <string.h>

#define AES256_KEY_SIZE   32
#define AES256_IV_SIZE    16
#define AES256_BLOCK_SIZE 16

struct xylem_aes256_s {
    struct AES_ctx aes;
    uint8_t        key[AES256_KEY_SIZE];
};

static void _aes256_generate_iv(uint8_t iv[AES256_IV_SIZE]) {
    platform_info_getrandom(iv, AES256_IV_SIZE);
}

size_t xylem_aes256_ctr_encrypt_size(size_t len) {
    return AES256_IV_SIZE + len;
}

size_t xylem_aes256_ctr_decrypt_size(size_t len) {
    if (len < AES256_IV_SIZE) {
        return 0;
    }
    return len - AES256_IV_SIZE;
}

size_t xylem_aes256_cbc_encrypt_size(size_t len) {
    size_t pad = AES256_BLOCK_SIZE - (len % AES256_BLOCK_SIZE);
    return AES256_IV_SIZE + len + pad;
}

size_t xylem_aes256_cbc_decrypt_size(size_t len) {
    if (len < AES256_IV_SIZE + AES256_BLOCK_SIZE) {
        return 0;
    }
    return len - AES256_IV_SIZE;
}

xylem_aes256_t* xylem_aes256_create(const uint8_t key[32]) {
    xylem_aes256_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    memcpy(ctx->key, key, AES256_KEY_SIZE);
    AES_init_ctx(&ctx->aes, key);
    return ctx;
}

void xylem_aes256_destroy(xylem_aes256_t* ctx) {
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    free(ctx);
}

int xylem_aes256_ctr_encrypt(xylem_aes256_t* ctx, const uint8_t* src,
                             size_t slen, uint8_t* dst, size_t dlen) {
    if (!ctx || !dst) {
        return -1;
    }
    size_t need = xylem_aes256_ctr_encrypt_size(slen);
    if (dlen < need) {
        return -1;
    }
    uint8_t iv[AES256_IV_SIZE];
    _aes256_generate_iv(iv);
    memcpy(dst, iv, AES256_IV_SIZE);
    if (slen > 0) {
        memcpy(dst + AES256_IV_SIZE, src, slen);
    }
    AES_init_ctx_iv(&ctx->aes, ctx->key, iv);
    AES_CTR_xcrypt_buffer(&ctx->aes, dst + AES256_IV_SIZE, slen);
    return (int)need;
}

int xylem_aes256_ctr_decrypt(xylem_aes256_t* ctx, const uint8_t* src,
                             size_t slen, uint8_t* dst, size_t dlen) {
    if (!ctx || !dst || slen < AES256_IV_SIZE) {
        return -1;
    }
    size_t ct_len = slen - AES256_IV_SIZE;
    if (dlen < ct_len) {
        return -1;
    }
    AES_init_ctx_iv(&ctx->aes, ctx->key, src);
    if (ct_len > 0) {
        memcpy(dst, src + AES256_IV_SIZE, ct_len);
    }
    AES_CTR_xcrypt_buffer(&ctx->aes, dst, ct_len);
    return (int)ct_len;
}

int xylem_aes256_cbc_encrypt(xylem_aes256_t* ctx, const uint8_t* src,
                             size_t slen, uint8_t* dst, size_t dlen) {
    if (!ctx || !dst) {
        return -1;
    }
    size_t need = xylem_aes256_cbc_encrypt_size(slen);
    if (dlen < need) {
        return -1;
    }
    uint8_t pad_val = (uint8_t)(AES256_BLOCK_SIZE -
                                (slen % AES256_BLOCK_SIZE));
    size_t padded_len = slen + pad_val;
    uint8_t iv[AES256_IV_SIZE];
    _aes256_generate_iv(iv);
    memcpy(dst, iv, AES256_IV_SIZE);
    if (slen > 0) {
        memcpy(dst + AES256_IV_SIZE, src, slen);
    }
    memset(dst + AES256_IV_SIZE + slen, pad_val, pad_val);
    AES_init_ctx_iv(&ctx->aes, ctx->key, iv);
    AES_CBC_encrypt_buffer(&ctx->aes, dst + AES256_IV_SIZE, padded_len);
    return (int)need;
}

int xylem_aes256_cbc_decrypt(xylem_aes256_t* ctx, const uint8_t* src,
                             size_t slen, uint8_t* dst, size_t dlen) {
    if (!ctx || !dst) {
        return -1;
    }
    size_t ct_len = slen - AES256_IV_SIZE;
    if (slen < AES256_IV_SIZE + AES256_BLOCK_SIZE ||
        ct_len % AES256_BLOCK_SIZE != 0) {
        return -1;
    }
    if (dlen < ct_len) {
        return -1;
    }
    AES_init_ctx_iv(&ctx->aes, ctx->key, src);
    memcpy(dst, src + AES256_IV_SIZE, ct_len);
    AES_CBC_decrypt_buffer(&ctx->aes, dst, ct_len);
    uint8_t pad_val = dst[ct_len - 1];
    if (pad_val == 0 || pad_val > AES256_BLOCK_SIZE) {
        return -1;
    }
    for (size_t i = 0; i < pad_val; i++) {
        if (dst[ct_len - 1 - i] != pad_val) {
            return -1;
        }
    }
    return (int)(ct_len - pad_val);
}
