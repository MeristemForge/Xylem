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
#include <time.h>

static void test_getcpus(void) {
    int cpus = platform_info_getcpus();
    ASSERT(cpus > 0);
}

static void test_getpid(void) {
    platform_pid_t pid = platform_info_getpid();
    ASSERT(pid > 0);
}

static void test_gettid(void) {
    platform_tid_t tid = platform_info_gettid();
    ASSERT(tid > 0);
}

static void test_getlocaltime(void) {
    time_t    now = time(NULL);
    struct tm tm;
    memset(&tm, 0, sizeof(tm));

    platform_info_getlocaltime(&now, &tm);

    ASSERT(tm.tm_year >= 125); /* 2025+ */
    ASSERT(tm.tm_mon >= 0 && tm.tm_mon <= 11);
    ASSERT(tm.tm_mday >= 1 && tm.tm_mday <= 31);
    ASSERT(tm.tm_hour >= 0 && tm.tm_hour <= 23);
    ASSERT(tm.tm_min >= 0 && tm.tm_min <= 59);
    ASSERT(tm.tm_sec >= 0 && tm.tm_sec <= 60);
}

int main(void) {
    test_getcpus();
    test_getpid();
    test_gettid();
    test_getlocaltime();
    return 0;
}
