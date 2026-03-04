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

#include "platform/platform.h"
#include "assert.h"
#include <string.h>

static void test_path_separator(void) {
#if defined(_WIN32)
    ASSERT(PLATFORM_PATH_SEPARATOR == '\\');
#else
    ASSERT(PLATFORM_PATH_SEPARATOR == '/');
#endif
}

static void test_fopen_write_read(void) {
    const char* path = "test-platform-io-tmp.txt";
    const char* msg  = "hello platform io";

    FILE* f = platform_io_fopen(path, "w");
    ASSERT(f != NULL);
    fprintf(f, "%s", msg);
    fclose(f);

    f = platform_io_fopen(path, "r");
    ASSERT(f != NULL);
    char buf[64] = {0};
    fgets(buf, sizeof(buf), f);
    fclose(f);

    ASSERT(strcmp(buf, msg) == 0);
    remove(path);
}

static void test_fopen_invalid(void) {
    /* opening a non-existent file for reading should return NULL */
    FILE* f = platform_io_fopen("__no_such_file_12345__", "r");
    ASSERT(f == NULL);
}

/* helper for test_vsprintf */
static int _format(char* buf, size_t size, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = platform_io_vsprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

static void test_vsprintf(void) {
    char buf[128] = {0};
    int  n = _format(buf, sizeof(buf), "num=%d str=%s", 42, "abc");
    ASSERT(n > 0);
    ASSERT(strcmp(buf, "num=42 str=abc") == 0);
}

int main(void) {
    test_path_separator();
    test_fopen_write_read();
    test_fopen_invalid();
    test_vsprintf();
    return 0;
}
