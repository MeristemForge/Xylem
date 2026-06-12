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

#include "xylem.h"
#include "assert.h"

#include <stdlib.h>
#include <string.h>

static void _has(xylem_json_t* j, const char* needle) {
    char* out = xylem_json_print(j);
    ASSERT(out != NULL);
    ASSERT(strstr(out, needle) != NULL);
    free(out);
}

static void test_parse_and_read(void) {
    const char* json = "{\"name\":\"xylem\",\"version\":3,\"active\":true}";
    xylem_json_t* j = xylem_json_parse(json);
    ASSERT(j != NULL);
    ASSERT(xylem_json_type(j) == XYLEM_JSON_TYPE_OBJ);

    const char* name = xylem_json_str(j, "name");
    ASSERT(name != NULL);
    ASSERT(strcmp(name, "xylem") == 0);
    ASSERT(xylem_json_i32(j, "version") == 3);
    ASSERT(xylem_json_bool(j, "active") == true);

    ASSERT(xylem_json_str(j, "missing") == NULL);
    ASSERT(xylem_json_i32(j, "missing") == 0);
    ASSERT(xylem_json_bool(j, "missing") == false);

    xylem_json_destroy(j);
}

static void test_parse_invalid(void) {
    ASSERT(xylem_json_parse(NULL) == NULL);
    ASSERT(xylem_json_parse("") == NULL);
    ASSERT(xylem_json_parse("{invalid}") == NULL);
}

static void test_parse_numbers(void) {
    const char* json = "{\"i32\":42,\"i64\":9876543210,\"f64\":3.14}";
    xylem_json_t* j = xylem_json_parse(json);
    ASSERT(j != NULL);

    ASSERT(xylem_json_i32(j, "i32") == 42);
    ASSERT(xylem_json_i64(j, "i64") == 9876543210LL);
    double f = xylem_json_f64(j, "f64");
    ASSERT(f > 3.13 && f < 3.15);

    xylem_json_destroy(j);
}

static void test_parse_nested_obj(void) {
    const char* json = "{\"info\":{\"city\":\"shanghai\",\"code\":200}}";
    xylem_json_t* j = xylem_json_parse(json);
    ASSERT(j != NULL);

    xylem_json_t* info = xylem_json_obj(j, "info");
    ASSERT(info != NULL);
    ASSERT(xylem_json_type(info) == XYLEM_JSON_TYPE_OBJ);

    const char* city = xylem_json_str(info, "city");
    ASSERT(city != NULL);
    ASSERT(strcmp(city, "shanghai") == 0);
    ASSERT(xylem_json_i32(info, "code") == 200);

    xylem_json_destroy(j);
}

static void test_parse_array(void) {
    const char* json = "{\"items\":[10,20,30]}";
    xylem_json_t* j = xylem_json_parse(json);
    ASSERT(j != NULL);

    xylem_json_t* arr = xylem_json_arr(j, "items");
    ASSERT(arr != NULL);
    ASSERT(xylem_json_type(arr) == XYLEM_JSON_TYPE_ARR);
    ASSERT(xylem_json_arr_len(arr) == 3);

    xylem_json_t* elem = xylem_json_arr_get(arr, 1);
    ASSERT(elem != NULL);
    ASSERT(xylem_json_type(elem) == XYLEM_JSON_TYPE_NUM);
    ASSERT(xylem_json_arr_get(arr, 99) == NULL);

    xylem_json_destroy(j);
}

static void test_parse_root_array(void) {
    const char* json = "[\"a\",\"b\",\"c\"]";
    xylem_json_t* j = xylem_json_parse(json);
    ASSERT(j != NULL);
    ASSERT(xylem_json_type(j) == XYLEM_JSON_TYPE_ARR);
    ASSERT(xylem_json_arr_len(j) == 3);

    xylem_json_destroy(j);
}

static void test_print(void) {
    const char* json = "{\"a\":1}";
    xylem_json_t* j = xylem_json_parse(json);
    ASSERT(j != NULL);

    char* out = xylem_json_print(j);
    ASSERT(out != NULL);
    ASSERT(strcmp(out, "{\"a\":1}") == 0);
    free(out);

    char* pretty = xylem_json_print_pretty(j);
    ASSERT(pretty != NULL);
    ASSERT(strstr(pretty, "\n") != NULL);
    free(pretty);

    xylem_json_destroy(j);
}

static void test_build_obj(void) {
    xylem_json_t* j = xylem_json_new_obj();
    ASSERT(j != NULL);
    ASSERT(xylem_json_type(j) == XYLEM_JSON_TYPE_OBJ);

    ASSERT(xylem_json_set_str(j, "name", "xylem") == 0);
    ASSERT(xylem_json_set_i32(j, "major", 1) == 0);
    ASSERT(xylem_json_set_i64(j, "big", 9876543210LL) == 0);
    ASSERT(xylem_json_set_f64(j, "pi", 3.14) == 0);
    ASSERT(xylem_json_set_bool(j, "ok", true) == 0);
    ASSERT(xylem_json_set_null(j, "empty") == 0);

    _has(j, "\"name\":\"xylem\"");
    _has(j, "\"major\":1");
    _has(j, "\"ok\":true");
    _has(j, "\"empty\":null");

    xylem_json_destroy(j);
}

static void test_build_arr(void) {
    xylem_json_t* arr = xylem_json_new_arr();
    ASSERT(arr != NULL);
    ASSERT(xylem_json_type(arr) == XYLEM_JSON_TYPE_ARR);

    ASSERT(xylem_json_arr_push_str(arr, "hello") == 0);
    ASSERT(xylem_json_arr_push_i64(arr, 42) == 0);
    ASSERT(xylem_json_arr_push_f64(arr, 2.5) == 0);
    ASSERT(xylem_json_arr_push_bool(arr, false) == 0);

    _has(arr, "\"hello\"");
    _has(arr, "42");
    _has(arr, "false");

    xylem_json_destroy(arr);
}

static void test_build_nested(void) {
    xylem_json_t* root = xylem_json_new_obj();
    ASSERT(root != NULL);

    xylem_json_t* child = xylem_json_new_obj();
    ASSERT(child != NULL);
    ASSERT(xylem_json_set_str(child, "k", "v") == 0);
    ASSERT(xylem_json_set_obj(root, "nested", child) == 0);

    xylem_json_t* arr = xylem_json_new_arr();
    ASSERT(arr != NULL);
    ASSERT(xylem_json_arr_push_i64(arr, 1) == 0);
    ASSERT(xylem_json_set_arr(root, "list", arr) == 0);

    _has(root, "\"nested\":{\"k\":\"v\"}");
    _has(root, "\"list\":[1]");

    xylem_json_destroy(root);
}

static void test_build_arr_push_obj(void) {
    xylem_json_t* arr = xylem_json_new_arr();
    ASSERT(arr != NULL);

    xylem_json_t* item = xylem_json_new_obj();
    ASSERT(item != NULL);
    ASSERT(xylem_json_set_i32(item, "id", 1) == 0);
    ASSERT(xylem_json_arr_push(arr, item) == 0);
    ASSERT(xylem_json_arr_len(arr) == 1);

    _has(arr, "{\"id\":1}");

    xylem_json_destroy(arr);
}

static void test_null_safety(void) {
    ASSERT(xylem_json_type(NULL) == XYLEM_JSON_TYPE_NONE);
    ASSERT(xylem_json_str(NULL, "k") == NULL);
    ASSERT(xylem_json_i32(NULL, "k") == 0);
    ASSERT(xylem_json_i64(NULL, "k") == 0);
    ASSERT(xylem_json_f64(NULL, "k") == 0.0);
    ASSERT(xylem_json_bool(NULL, "k") == false);
    ASSERT(xylem_json_obj(NULL, "k") == NULL);
    ASSERT(xylem_json_arr(NULL, "k") == NULL);
    ASSERT(xylem_json_arr_len(NULL) == 0);
    ASSERT(xylem_json_arr_get(NULL, 0) == NULL);
    ASSERT(xylem_json_print(NULL) == NULL);
    ASSERT(xylem_json_print_pretty(NULL) == NULL);

    xylem_json_destroy(NULL);

    ASSERT(xylem_json_set_str(NULL, "k", "v") == -1);
    ASSERT(xylem_json_set_i32(NULL, "k", 0) == -1);
    ASSERT(xylem_json_arr_push_str(NULL, "v") == -1);
}

static void test_type_enum(void) {
    const char* json =
        "{\"s\":\"x\",\"n\":1,\"b\":true,\"nil\":null,"
        "\"a\":[],\"o\":{}}";
    xylem_json_t* j = xylem_json_parse(json);
    ASSERT(j != NULL);

    xylem_json_t* a = xylem_json_arr(j, "a");
    ASSERT(a != NULL);
    ASSERT(xylem_json_type(a) == XYLEM_JSON_TYPE_ARR);

    xylem_json_t* o = xylem_json_obj(j, "o");
    ASSERT(o != NULL);
    ASSERT(xylem_json_type(o) == XYLEM_JSON_TYPE_OBJ);

    xylem_json_destroy(j);
}

int main(void) {
    test_parse_and_read();
    test_parse_invalid();
    test_parse_numbers();
    test_parse_nested_obj();
    test_parse_array();
    test_parse_root_array();
    test_print();
    test_build_obj();
    test_build_arr();
    test_build_nested();
    test_build_arr_push_obj();
    test_null_safety();
    test_type_enum();
    return 0;
}
