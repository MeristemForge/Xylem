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

#include "platform/platform-info.h"

#include <fcntl.h>

int platform_info_getcpus(void) {
    return (int)sysconf(_SC_NPROCESSORS_ONLN);
}

platform_pid_t platform_info_getpid(void) {
    return getpid();
}

void platform_info_getlocaltime(
    const time_t* restrict time, struct tm* restrict tm) {
    tzset();
    localtime_r(time, tm);
}

void platform_info_gmtime(const time_t* t, struct tm* tm) {
    gmtime_r(t, tm);
}

time_t platform_info_mkgmtime(struct tm* tm) {
    return timegm(tm);
}

#if defined(__APPLE__)
platform_tid_t platform_info_gettid(void) {
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    return tid;
}
#endif

#if defined(__linux__)
platform_tid_t platform_info_gettid(void) {
    return syscall(SYS_gettid);
}
#endif

bool platform_info_getrandom(void* buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return false;
    }
    ssize_t n = read(fd, buf, len);
    close(fd);
    return n == (ssize_t)len;
}
