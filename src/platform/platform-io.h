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

#include <stdio.h>
#include <stdarg.h>

#if defined(_WIN32)
#define PLATFORM_PATH_SEPARATOR '\\'
#else
#define PLATFORM_PATH_SEPARATOR '/'
#endif

/**
 * @brief Open a file with the given mode (portable wrapper).
 *
 * Uses _fsopen on MSVC for shared access, fopen elsewhere.
 *
 * @param file  Path to the file.
 * @param mode  Open mode string (e.g. "r", "wb").
 *
 * @return FILE pointer on success, NULL on failure.
 */
extern FILE* platform_io_fopen(const char* restrict file, const char* restrict mode);

/**
 * @brief Format a string into a buffer (portable wrapper).
 *
 * Uses vsnprintf on all platforms.
 *
 * @param str     Destination buffer.
 * @param size    Size of the destination buffer in bytes.
 * @param format  printf-style format string.
 * @param ap      Variable argument list.
 *
 * @return Number of characters written (excluding null terminator),
 *         or a negative value on error.
 */
extern int platform_io_vsprintf(char* str, size_t size, const char* restrict format, va_list ap);
