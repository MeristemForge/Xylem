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

typedef struct xylem_http_multipart_part_s {
    const char* name;         /* Content-Disposition "name" value. */
    size_t      name_len;     /* Length of name. */
    const char* filename;     /* Content-Disposition "filename", or NULL. */
    size_t      filename_len; /* Length of filename. */
    const char* content_type; /* Content-Type of this part, or NULL. */
    size_t      content_type_len; /* Length of content_type. */
    const void* body;         /* Part body data (points into source). */
    size_t      body_len;     /* Length of body. */
} xylem_http_multipart_part_t;

/**
 * @brief Parse multipart/form-data body.
 *
 * @note [COROUTINE-ONLY]
 *
 * All returned pointers point into the original data buffer (zero-copy).
 *
 * @param data       Multipart body bytes.
 * @param data_len   Length of data.
 * @param boundary   Boundary string (without leading "--").
 * @param parts      Output array of parts.
 * @param max_parts  Capacity of parts array.
 *
 * @return Number of parts parsed, or -1 on error.
 */
extern int xylem_http_multipart_parse(
    const void*                  data,
    size_t                       data_len,
    const char*                  boundary,
    xylem_http_multipart_part_t* parts,
    int                          max_parts);

/**
 * @brief Parse multipart body directly from an HTTP request.
 *
 * @note [COROUTINE-ONLY]
 *
 * Extracts boundary from Content-Type header automatically.
 *
 * @param req        Request handle.
 * @param parts      Output array of parts.
 * @param max_parts  Capacity of parts array.
 *
 * @return Number of parts parsed, or -1 on error.
 */
extern int xylem_http_multipart_parse_request(
    const xylem_http_req_t*      req,
    xylem_http_multipart_part_t* parts,
    int                          max_parts);

/**
 * @brief Extract boundary from a Content-Type header value.
 *
 * @note [COROUTINE-ONLY]
 *
 * @param content_type  Content-Type header value string.
 * @param len           Output: length of boundary string.
 *
 * @return Pointer into content_type at boundary start, or NULL if not found.
 */
extern const char* xylem_http_multipart_boundary(
    const char* content_type,
    size_t*     len);
