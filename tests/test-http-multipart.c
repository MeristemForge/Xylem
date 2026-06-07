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

#include "xylem/net/http/xylem-http-multipart.h"
#include "assert.h"

#include <string.h>

static const char TEST_BODY[] =
    "--abc123\r\n"
    "Content-Disposition: form-data; name=\"field1\"\r\n"
    "\r\n"
    "value1\r\n"
    "--abc123\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "file content here\r\n"
    "--abc123--\r\n";

static void test_parse_basic(void) {
    xylem_http_multipart_part_t parts[4];
    int n = xylem_http_multipart_parse(
        TEST_BODY, sizeof(TEST_BODY) - 1, "abc123", parts, 4);
    ASSERT(n == 2);

    ASSERT(parts[0].name_len == 6);
    ASSERT(memcmp(parts[0].name, "field1", 6) == 0);
    ASSERT(parts[0].filename == NULL);
    ASSERT(parts[0].body_len == 6);
    ASSERT(memcmp(parts[0].body, "value1", 6) == 0);

    ASSERT(parts[1].name_len == 4);
    ASSERT(memcmp(parts[1].name, "file", 4) == 0);
    ASSERT(parts[1].filename_len == 8);
    ASSERT(memcmp(parts[1].filename, "test.txt", 8) == 0);
    ASSERT(parts[1].content_type_len == 10);
    ASSERT(memcmp(parts[1].content_type, "text/plain", 10) == 0);
    ASSERT(parts[1].body_len == 17);
    ASSERT(memcmp(parts[1].body, "file content here", 17) == 0);
}

static void test_parse_max_parts(void) {
    xylem_http_multipart_part_t parts[1];
    int n = xylem_http_multipart_parse(
        TEST_BODY, sizeof(TEST_BODY) - 1, "abc123", parts, 1);
    ASSERT(n == 1);
    ASSERT(parts[0].name_len == 6);
    ASSERT(memcmp(parts[0].name, "field1", 6) == 0);
}

static void test_boundary_extract(void) {
    size_t len;
    const char* b;

    b = xylem_http_multipart_boundary(
        "multipart/form-data; boundary=abc123", &len);
    ASSERT(b != NULL);
    ASSERT(len == 6);
    ASSERT(memcmp(b, "abc123", 6) == 0);

    b = xylem_http_multipart_boundary(
        "multipart/form-data; boundary=\"quoted-val\"", &len);
    ASSERT(b != NULL);
    ASSERT(len == 10);
    ASSERT(memcmp(b, "quoted-val", 10) == 0);

    b = xylem_http_multipart_boundary("text/plain", &len);
    ASSERT(b == NULL);
}

static void test_parse_empty(void) {
    xylem_http_multipart_part_t parts[4];
    int n = xylem_http_multipart_parse(NULL, 0, "abc", parts, 4);
    ASSERT(n == -1);

    n = xylem_http_multipart_parse("x", 1, "abc", parts, 4);
    ASSERT(n == -1);
}

static void test_parse_no_parts(void) {
    const char body[] = "--abc--\r\n";
    xylem_http_multipart_part_t parts[4];
    int n = xylem_http_multipart_parse(body, sizeof(body) - 1, "abc", parts, 4);
    ASSERT(n == 0);
}

int main(void) {
    test_parse_basic();
    test_parse_max_parts();
    test_boundary_extract();
    test_parse_empty();
    test_parse_no_parts();
    return 0;
}
