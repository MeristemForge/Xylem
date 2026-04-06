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
#include "xylem/xylem-aes256.h"

#include <string.h>

static const uint8_t _test_key[32] = {
    0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe,
    0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
    0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7,
    0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4,
};

static void test_ctr_round_trip(void) {
    xylem_aes256_t* ctx = xylem_aes256_create(_test_key);
    ASSERT(ctx != NULL);

    const char* plaintext = "Hello, AES-256-CTR!";
    size_t pt_len = strlen(plaintext);
    size_t enc_size = xylem_aes256_ctr_encrypt_size(pt_len);

    uint8_t encrypted[256];
    int enc_len = xylem_aes256_ctr_encrypt(ctx, (const uint8_t*)plaintext,
                                           pt_len, encrypted, enc_size);
    ASSERT(enc_len == (int)enc_size);

    uint8_t decrypted[256];
    size_t dec_size = xylem_aes256_ctr_decrypt_size((size_t)enc_len);
    int dec_len = xylem_aes256_ctr_decrypt(ctx, encrypted, (size_t)enc_len,
                                           decrypted, dec_size);
    ASSERT(dec_len == (int)pt_len);
    ASSERT(memcmp(decrypted, plaintext, pt_len) == 0);

    xylem_aes256_destroy(ctx);
}

static void test_cbc_round_trip(void) {
    xylem_aes256_t* ctx = xylem_aes256_create(_test_key);
    ASSERT(ctx != NULL);

    const char* plaintext = "Hello, AES-256-CBC!";
    size_t pt_len = strlen(plaintext);
    size_t enc_size = xylem_aes256_cbc_encrypt_size(pt_len);

    uint8_t encrypted[256];
    int enc_len = xylem_aes256_cbc_encrypt(ctx, (const uint8_t*)plaintext,
                                           pt_len, encrypted, enc_size);
    ASSERT(enc_len == (int)enc_size);

    uint8_t decrypted[256];
    size_t dec_size = xylem_aes256_cbc_decrypt_size((size_t)enc_len);
    int dec_len = xylem_aes256_cbc_decrypt(ctx, encrypted, (size_t)enc_len,
                                           decrypted, dec_size);
    ASSERT(dec_len == (int)pt_len);
    ASSERT(memcmp(decrypted, plaintext, pt_len) == 0);

    xylem_aes256_destroy(ctx);
}

static void test_cbc_block_aligned(void) {
    xylem_aes256_t* ctx = xylem_aes256_create(_test_key);
    ASSERT(ctx != NULL);

    const uint8_t plaintext[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    size_t enc_size = xylem_aes256_cbc_encrypt_size(16);

    uint8_t encrypted[256];
    int enc_len = xylem_aes256_cbc_encrypt(ctx, plaintext, 16,
                                           encrypted, enc_size);
    ASSERT(enc_len == (int)enc_size);

    uint8_t decrypted[256];
    size_t dec_size = xylem_aes256_cbc_decrypt_size((size_t)enc_len);
    int dec_len = xylem_aes256_cbc_decrypt(ctx, encrypted, (size_t)enc_len,
                                           decrypted, dec_size);
    ASSERT(dec_len == 16);
    ASSERT(memcmp(decrypted, plaintext, 16) == 0);

    xylem_aes256_destroy(ctx);
}

static void test_ctr_empty(void) {
    xylem_aes256_t* ctx = xylem_aes256_create(_test_key);
    ASSERT(ctx != NULL);

    size_t enc_size = xylem_aes256_ctr_encrypt_size(0);
    uint8_t encrypted[256];
    int enc_len = xylem_aes256_ctr_encrypt(ctx, NULL, 0,
                                           encrypted, enc_size);
    ASSERT(enc_len == (int)enc_size);

    uint8_t decrypted[256];
    int dec_len = xylem_aes256_ctr_decrypt(ctx, encrypted, (size_t)enc_len,
                                           decrypted, 256);
    ASSERT(dec_len == 0);

    xylem_aes256_destroy(ctx);
}

static void test_cbc_invalid_padding(void) {
    xylem_aes256_t* ctx = xylem_aes256_create(_test_key);
    ASSERT(ctx != NULL);

    uint8_t fake[32];
    memset(fake, 0x41, sizeof(fake));

    uint8_t out[256];
    int rc = xylem_aes256_cbc_decrypt(ctx, fake, sizeof(fake), out, 256);
    ASSERT(rc == -1);

    xylem_aes256_destroy(ctx);
}

static void test_ctr_insufficient_buffer(void) {
    xylem_aes256_t* ctx = xylem_aes256_create(_test_key);
    ASSERT(ctx != NULL);

    const uint8_t msg[] = "test";
    uint8_t out[4];
    /* Buffer too small: need 20 bytes (16 IV + 4 data), only 4. */
    int rc = xylem_aes256_ctr_encrypt(ctx, msg, 4, out, 4);
    ASSERT(rc == -1);

    xylem_aes256_destroy(ctx);
}

static void test_ctr_different_iv_each_time(void) {
    xylem_aes256_t* ctx = xylem_aes256_create(_test_key);
    ASSERT(ctx != NULL);

    const uint8_t msg[] = "same message";
    size_t enc_size = xylem_aes256_ctr_encrypt_size(sizeof(msg));
    uint8_t enc1[256], enc2[256];

    xylem_aes256_ctr_encrypt(ctx, msg, sizeof(msg), enc1, enc_size);
    xylem_aes256_ctr_encrypt(ctx, msg, sizeof(msg), enc2, enc_size);

    /* IVs (first 16 bytes) should differ. */
    ASSERT(memcmp(enc1, enc2, 16) != 0);

    xylem_aes256_destroy(ctx);
}

int main(void) {
    test_ctr_round_trip();
    test_cbc_round_trip();
    test_cbc_block_aligned();
    test_ctr_empty();
    test_cbc_invalid_padding();
    test_ctr_insufficient_buffer();
    test_ctr_different_iv_each_time();
    return 0;
}
