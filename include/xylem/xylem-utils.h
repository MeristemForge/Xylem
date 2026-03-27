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

typedef enum xylem_endian_e         xylem_endian_t;
typedef enum xylem_time_precision_e xylem_time_precision_t;

enum xylem_endian_e {
    XYLEM_ENDIAN_BE = 0,
    XYLEM_ENDIAN_LE = 1,
};

enum xylem_time_precision_e {
    XYLEM_TIME_PRECISION_SEC,
    XYLEM_TIME_PRECISION_MSEC,
    XYLEM_TIME_PRECISION_USEC,
    XYLEM_TIME_PRECISION_NSEC,
};

/**
 * @brief Get a pseudo-random number in the given range.
 *
 * @param min  Minimum value (inclusive).
 * @param max  Maximum value (inclusive).
 *
 * @return Random integer in [min, max].
 */
extern int xylem_utils_getprng(int min, int max);

/**
 * @brief Get the current time with specified precision.
 *
 * @param precision  Time precision (sec, msec, usec, nsec).
 *
 * @return Current time in the requested unit.
 */
extern uint64_t xylem_utils_getnow(xylem_time_precision_t precision);

/**
 * @brief Detect the system byte order.
 *
 * @return XYLEM_ENDIAN_LE for little-endian, XYLEM_ENDIAN_BE for big-endian.
 */
extern xylem_endian_t xylem_utils_getendian(void);
