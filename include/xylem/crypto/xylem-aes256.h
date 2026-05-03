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

typedef struct xylem_aes256_s xylem_aes256_t;

/**
 * @brief Compute the output size for CTR encryption.
 *
 * @param len  Plaintext length in bytes.
 *
 * @return Required output buffer size.
 */
extern size_t xylem_aes256_ctr_encrypt_size(size_t len);

/**
 * @brief Compute the maximum output size for CTR decryption.
 *
 * @param len  Ciphertext length in bytes (including IV prefix).
 *
 * @return Maximum plaintext output size, or 0 if len is too small.
 */
extern size_t xylem_aes256_ctr_decrypt_size(size_t len);

/**
 * @brief Compute the output size for CBC encryption.
 *
 * Accounts for the prepended IV and PKCS7 padding.
 *
 * @param len  Plaintext length in bytes.
 *
 * @return Required output buffer size.
 */
extern size_t xylem_aes256_cbc_encrypt_size(size_t len);

/**
 * @brief Compute the maximum output size for CBC decryption.
 *
 * The actual output may be smaller after PKCS7 unpadding.
 *
 * @param len  Ciphertext length in bytes (including IV prefix).
 *
 * @return Maximum plaintext output size, or 0 if len is too small.
 */
extern size_t xylem_aes256_cbc_decrypt_size(size_t len);

/**
 * @brief Create an AES-256 context.
 *
 * Performs key expansion once. The context can be reused for
 * multiple encrypt/decrypt operations with the same key.
 *
 * @param key  32-byte secret key.
 *
 * @return Context handle, or NULL on failure.
 */
extern xylem_aes256_t* xylem_aes256_create(const uint8_t key[32]);

/**
 * @brief Destroy an AES-256 context and zero sensitive data.
 *
 * @param ctx  Context handle.
 */
extern void xylem_aes256_destroy(xylem_aes256_t* ctx);

/**
 * @brief Encrypt data using AES-256-CTR.
 *
 * A random 16-byte IV is generated internally and prepended to the
 * Use xylem_aes256_ctr_encrypt_size() to determine the required dlen.
 *
 * @param ctx   Context handle.
 * @param src   Plaintext input.
 * @param slen  Input length in bytes (any length).
 * @param dst   Output buffer (IV + ciphertext).
 * @param dlen  Size of the output buffer in bytes.
 *
 * @return Number of bytes written to dst on success, -1 on failure.
 */
extern int xylem_aes256_ctr_encrypt(xylem_aes256_t* ctx,
                                    const uint8_t* src, size_t slen,
                                    uint8_t* dst, size_t dlen);

/**
 * @brief Decrypt data using AES-256-CTR.
 *
 * Reads the IV from the front of the input, then decrypts the
 * remaining ciphertext. Use xylem_aes256_ctr_decrypt_size() to
 * determine the required dlen.
 *
 * @param ctx   Context handle.
 * @param src   Input buffer (IV + ciphertext).
 * @param slen  Input length in bytes.
 * @param dst   Plaintext output buffer.
 * @param dlen  Size of the output buffer in bytes.
 *
 * @return Number of bytes written to dst on success, -1 on failure.
 */
extern int xylem_aes256_ctr_decrypt(xylem_aes256_t* ctx,
                                    const uint8_t* src, size_t slen,
                                    uint8_t* dst, size_t dlen);

/**
 * @brief Encrypt data using AES-256-CBC with PKCS7 padding.
 *
 * A random IV is generated internally and prepended to the output.
 * PKCS7 padding is applied automatically. Use
 * xylem_aes256_cbc_encrypt_size() to determine the required dlen.
 *
 * @param ctx   Context handle.
 * @param src   Plaintext input.
 * @param slen  Input length in bytes (any length).
 * @param dst   Output buffer (IV + padded ciphertext).
 * @param dlen  Size of the output buffer in bytes.
 *
 * @return Number of bytes written to dst on success, -1 on failure.
 */
extern int xylem_aes256_cbc_encrypt(xylem_aes256_t* ctx,
                                    const uint8_t* src, size_t slen,
                                    uint8_t* dst, size_t dlen);

/**
 * @brief Decrypt data using AES-256-CBC with PKCS7 unpadding.
 *
 * Reads the IV from the front of the input, decrypts the remaining
 * ciphertext, and removes PKCS7 padding. Use
 * xylem_aes256_cbc_decrypt_size() to determine the required dlen.
 *
 * @param ctx   Context handle.
 * @param src   Input buffer (IV + padded ciphertext).
 * @param slen  Input length in bytes.
 * @param dst   Plaintext output buffer.
 * @param dlen  Size of the output buffer in bytes.
 *
 * @return Number of bytes written to dst on success, -1 on failure
 *         (insufficient dlen, invalid padding, or bad input length).
 */
extern int xylem_aes256_cbc_decrypt(xylem_aes256_t* ctx,
                                    const uint8_t* src, size_t slen,
                                    uint8_t* dst, size_t dlen);
