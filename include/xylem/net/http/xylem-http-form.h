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

#include "xylem/net/http/xylem-http.h"

#include <stddef.h>

/**
 * @brief Compute the maximum number of key-value pairs in a form body.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param body      Form body string.
 * @param body_len  Length of body.
 *
 * @return Maximum pair count (number of '&' + 1), or 0 if body is NULL/empty.
 */
extern int xylem_http_form_count(const char* body, size_t body_len);

/**
 * @brief Parse application/x-www-form-urlencoded body in-place.
 *
 * @note [COROUTINE-ONLY]
 *
 * Modifies buf by inserting '\0' terminators and decoding %XX sequences.
 * Output pairs point directly into buf. Handles '+' as space.
 *
 * @param buf        Writable copy of the form body.
 * @param buf_len    Length of buf (excluding any trailing '\0').
 * @param pairs      Output array of key-value pairs.
 * @param max_pairs  Capacity of pairs array.
 *
 * @return Number of pairs parsed, or -1 on error.
 */
extern int xylem_http_form_parse(
    char*             buf,
    size_t            buf_len,
    xylem_http_hdr_t* pairs,
    int               max_pairs);

/**
 * @brief Find a value by key in parsed form pairs.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param pairs  Parsed key-value pairs array.
 * @param count  Number of pairs.
 * @param key    Key to search for.
 *
 * @return Value string, or NULL if not found.
 */
extern const char* xylem_http_form_get(
    const xylem_http_hdr_t* pairs,
    int                     count,
    const char*             key);
