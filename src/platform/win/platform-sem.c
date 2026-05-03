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

#include <stdlib.h>

platform_sem_t* platform_sem_create(unsigned int value) {
    platform_sem_t* sem = (platform_sem_t*)malloc(sizeof(platform_sem_t));
    if (!sem) {
        return NULL;
    }
    *sem = CreateSemaphoreW(NULL, (LONG)value, LONG_MAX, NULL);
    if (!*sem) {
        free(sem);
        return NULL;
    }
    return sem;
}

void platform_sem_destroy(platform_sem_t* sem) {
    CloseHandle(*sem);
    free(sem);
}

void platform_sem_post(platform_sem_t* sem) {
    ReleaseSemaphore(*sem, 1, NULL);
}

void platform_sem_wait(platform_sem_t* sem) {
    WaitForSingleObject(*sem, INFINITE);
}
