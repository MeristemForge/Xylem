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

#include <stdint.h>

/**
 * @brief Byte-swap a value of various integer or floating-point types.
 *
 * This type-generic macro automatically selects the appropriate byte-swap
 * function based on the type of the input argument. It supports signed and
 * unsigned integers (16-, 32-, and 64-bit) as well as IEEE 754 floating-point
 * types (float and double). The operation reverses the byte order of the
 * underlying representation, which is useful for converting between little-
 * and big-endian formats.
 *
 * @param x  Input value to be byte-swapped. Must be of one of the supported
 * types: uint16_t, uint32_t, uint64_t, int16_t, int32_t, int64_t, float, or
 * double.
 *
 * @return The byte-swapped value of the same type as the input.
 *
 * @note For floating-point types, the function reinterprets the bit pattern as
 * an unsigned integer, performs the byte swap, and safely reconstructs the
 *       floating-point value using memcpy to avoid undefined behavior.
 */
#define xylem_bswap(x)                                                         \
    _Generic((x), uint16_t                                                     \
             : xylem_bswap_u16, uint32_t                                       \
             : xylem_bswap_u32, uint64_t                                       \
             : xylem_bswap_u64, int16_t                                        \
             : xylem_bswap_i16, int32_t                                        \
             : xylem_bswap_i32, int64_t                                        \
             : xylem_bswap_i64, float                                          \
             : xylem_bswap_f32, double                                         \
             : xylem_bswap_f64)(x)

/**
 * @brief Byte-swap a 16-bit unsigned integer.
 *
 * Reverses the order of the two bytes in the input value.
 *
 * @param val  Input 16-bit unsigned integer.
 * @return     Byte-swapped 16-bit unsigned integer.
 */
extern uint16_t xylem_bswap_u16(uint16_t val);

/**
 * @brief Byte-swap a 32-bit unsigned integer.
 *
 * Reverses the order of the four bytes in the input value.
 *
 * @param val  Input 32-bit unsigned integer.
 * @return     Byte-swapped 32-bit unsigned integer.
 */
extern uint32_t xylem_bswap_u32(uint32_t val);

/**
 * @brief Byte-swap a 64-bit unsigned integer.
 *
 * Reverses the order of the eight bytes in the input value.
 *
 * @param val  Input 64-bit unsigned integer.
 * @return     Byte-swapped 64-bit unsigned integer.
 */
extern uint64_t xylem_bswap_u64(uint64_t val);

/**
 * @brief Byte-swap a 16-bit signed integer.
 *
 * Performs byte swapping by treating the value as unsigned,
 * then casting the result back to a signed type.
 *
 * @param val  Input 16-bit signed integer.
 * @return     Byte-swapped 16-bit signed integer.
 */
extern int16_t xylem_bswap_i16(int16_t val);

/**
 * @brief Byte-swap a 32-bit signed integer.
 *
 * Internally uses the unsigned 32-bit byte-swap routine and
 * safely returns the result as a signed integer.
 *
 * @param val  Input 32-bit signed integer.
 * @return     Byte-swapped 32-bit signed integer.
 */
extern int32_t xylem_bswap_i32(int32_t val);

/**
 * @brief Byte-swap a 64-bit signed integer.
 *
 * Reinterprets the signed value as unsigned for byte swapping,
 * then casts the result back to a signed 64-bit integer.
 *
 * @param val  Input 64-bit signed integer.
 * @return     Byte-swapped 64-bit signed integer.
 */
extern int64_t xylem_bswap_i64(int64_t val);

/**
 * @brief Byte-swap a 32-bit IEEE 754 floating-point number.
 *
 * Safely reinterprets the float as a uint32_t using memcpy,
 * swaps its bytes, and reconstructs the float without violating
 * strict aliasing rules.
 *
 * @param val  Input 32-bit floating-point value.
 * @return     Byte-swapped float with reversed byte order.
 */
extern float xylem_bswap_f32(float val);

/**
 * @brief Byte-swap a 64-bit IEEE 754 floating-point number.
 *
 * Uses memcpy to copy the double into a uint64_t, performs byte
 * swapping, and copies the result back to a double in a portable
 * and standards-compliant way.
 *
 * @param val  Input 64-bit floating-point value.
 * @return     Byte-swapped double with reversed byte order.
 */
extern double xylem_bswap_f64(double val);