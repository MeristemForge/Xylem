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

#include "xylem/net/http/xylem-http-form.h"
#include "xylem.h"
#include "assert.h"
#include "utils.h"

#include <string.h>

static void test_count_basic(void) {
    ASSERT(xylem_http_form_count("a=1&b=2&c=3", 11) == 3);
    ASSERT(xylem_http_form_count("key=val", 7) == 1);
    ASSERT(xylem_http_form_count(NULL, 0) == 0);
    ASSERT(xylem_http_form_count("", 0) == 0);
}

static void test_parse_simple(void) {
    char buf[] = "user=alice&pass=secret";
    xylem_http_hdr_t pairs[4];
    int n = xylem_http_form_parse(buf, strlen(buf), pairs, 4);
    ASSERT(n == 2);
    ASSERT(strcmp(pairs[0].name, "user") == 0);
    ASSERT(strcmp(pairs[0].value, "alice") == 0);
    ASSERT(strcmp(pairs[1].name, "pass") == 0);
    ASSERT(strcmp(pairs[1].value, "secret") == 0);
}

static void test_parse_encoded(void) {
    char buf[] = "msg=hello+world&path=%2Ffoo%2Fbar";
    xylem_http_hdr_t pairs[4];
    int n = xylem_http_form_parse(buf, strlen(buf), pairs, 4);
    ASSERT(n == 2);
    ASSERT(strcmp(pairs[0].name, "msg") == 0);
    ASSERT(strcmp(pairs[0].value, "hello world") == 0);
    ASSERT(strcmp(pairs[1].name, "path") == 0);
    ASSERT(strcmp(pairs[1].value, "/foo/bar") == 0);
}

static void test_parse_empty_value(void) {
    char buf[] = "key=&other=val";
    xylem_http_hdr_t pairs[4];
    int n = xylem_http_form_parse(buf, strlen(buf), pairs, 4);
    ASSERT(n == 2);
    ASSERT(strcmp(pairs[0].name, "key") == 0);
    ASSERT(strcmp(pairs[0].value, "") == 0);
    ASSERT(strcmp(pairs[1].name, "other") == 0);
    ASSERT(strcmp(pairs[1].value, "val") == 0);
}

static void test_parse_no_equals(void) {
    char buf[] = "flag&key=val";
    xylem_http_hdr_t pairs[4];
    int n = xylem_http_form_parse(buf, strlen(buf), pairs, 4);
    ASSERT(n == 2);
    ASSERT(strcmp(pairs[0].name, "flag") == 0);
    ASSERT(strcmp(pairs[0].value, "") == 0);
}

static void test_parse_max_pairs(void) {
    char buf[] = "a=1&b=2&c=3&d=4";
    xylem_http_hdr_t pairs[2];
    int n = xylem_http_form_parse(buf, strlen(buf), pairs, 2);
    ASSERT(n == 2);
    ASSERT(strcmp(pairs[0].name, "a") == 0);
    ASSERT(strcmp(pairs[1].name, "b") == 0);
}

static void test_get(void) {
    char buf[] = "name=bob&age=30";
    xylem_http_hdr_t pairs[4];
    int n = xylem_http_form_parse(buf, strlen(buf), pairs, 4);
    ASSERT(n == 2);
    ASSERT(strcmp(xylem_http_form_get(pairs, n, "name"), "bob") == 0);
    ASSERT(strcmp(xylem_http_form_get(pairs, n, "age"), "30") == 0);
    ASSERT(xylem_http_form_get(pairs, n, "missing") == NULL);
}

static void _run_all(void* arg) {
    (void)arg;
    test_count_basic();
    test_parse_simple();
    test_parse_encoded();
    test_parse_empty_value();
    test_parse_no_equals();
    test_parse_max_pairs();
    test_get();
}

static void _test_run_all(void* arg) {
    (void)arg;
    _utils_watchdog_start(SAFETY_TIMEOUT_MS);

    _run_all(NULL);
    _utils_watchdog_stop();
    xylem_shutdown();
}

int main(void) {
    xylem_run(_test_run_all, NULL, NULL);
    return 0;
}
