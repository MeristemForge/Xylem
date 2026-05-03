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

#include "platform/platform-sem.h"

#include <errno.h>
#include <stdlib.h>

#if defined(__APPLE__)

platform_sem_t* platform_sem_create(unsigned int value) {
    platform_sem_t* sem = (platform_sem_t*)malloc(sizeof(platform_sem_t));
    if (!sem) {
        return NULL;
    }
    *sem = dispatch_semaphore_create((long)value);
    if (!*sem) {
        free(sem);
        return NULL;
    }
    return sem;
}

void platform_sem_destroy(platform_sem_t* sem) {
    dispatch_release(*sem);
    free(sem);
}

void platform_sem_post(platform_sem_t* sem) {
    dispatch_semaphore_signal(*sem);
}

void platform_sem_wait(platform_sem_t* sem) {
    dispatch_semaphore_wait(*sem, DISPATCH_TIME_FOREVER);
}

#endif

#if defined(__linux__)

platform_sem_t* platform_sem_create(unsigned int value) {
    platform_sem_t* sem = (platform_sem_t*)malloc(sizeof(platform_sem_t));
    if (!sem) {
        return NULL;
    }
    if (sem_init(sem, 0, value) != 0) {
        free(sem);
        return NULL;
    }
    return sem;
}

void platform_sem_destroy(platform_sem_t* sem) {
    sem_destroy(sem);
    free(sem);
}

void platform_sem_post(platform_sem_t* sem) {
    sem_post(sem);
}

void platform_sem_wait(platform_sem_t* sem) {
    int r;
    do {
        r = sem_wait(sem);
    } while (r == -1 && errno == EINTR);
}

#endif
