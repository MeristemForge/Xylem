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

#include "runtime/arena.h"

#include "xylem/xylem-logger.h"

#include "platform/platform-vmem.h"
#include "assert.h"

#include <stdint.h>

#define MIB (1024U * 1024U)

#ifdef ARENA_STANDALONE_TEST
void xylem_logger_log(
    xylem_logger_level_t level,
    const char* restrict file,
    int line,
    const char* restrict fmt,
    ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)fmt;
}
#endif

static void test_create_limits(void) {
    ASSERT(arena_create(0) == NULL);
    ASSERT(arena_create(MIB + 1U) == NULL);

    arena_t* arena = arena_create(MIB);
    ASSERT(arena != NULL);
    arena_destroy(arena);
}

static void test_alloc_free_realloc(void) {
    arena_t* arena = arena_create(123);
    ASSERT(arena != NULL);

    void*  slots[32] = {0};
    size_t page_size = platform_vmem_page_size();
    ASSERT(arena_alloc(arena, slots, 32) == 32);
    for (int i = 0; i < 32; i++) {
        ASSERT(slots[i] != NULL);
        ASSERT((uintptr_t)slots[i] % page_size == 0);
        for (int j = 0; j < i; j++) {
            ASSERT(slots[i] != slots[j]);
        }
        uint8_t* bytes = (uint8_t*)slots[i];
        bytes[0] = (uint8_t)i;
        bytes[122] = (uint8_t)(i + 1);
        ASSERT(bytes[0] == (uint8_t)i);
        ASSERT(bytes[122] == (uint8_t)(i + 1));
    }

    arena_free(arena, slots, 32);

    void* recycled[32] = {0};
    ASSERT(arena_alloc(arena, recycled, 32) == 32);
    for (int i = 0; i < 32; i++) {
        ASSERT(recycled[i] != NULL);
        uint8_t* bytes = (uint8_t*)recycled[i];
        bytes[0] = (uint8_t)(i + 2);
        bytes[122] = (uint8_t)(i + 3);
        ASSERT(bytes[0] == (uint8_t)(i + 2));
        ASSERT(bytes[122] == (uint8_t)(i + 3));
    }

    arena_free(arena, recycled, 32);
    arena_destroy(arena);
}

static void test_null_args(void) {
    void* slots[1] = {NULL};

    ASSERT(arena_alloc(NULL, slots, 1) == 0);

    arena_t* arena = arena_create(1);
    ASSERT(arena != NULL);
    ASSERT(arena_alloc(arena, NULL, 1) == 0);
    ASSERT(arena_alloc(arena, slots, 0) == 0);
    ASSERT(arena_alloc(arena, slots, -1) == 0);

    arena_free(NULL, slots, 1);
    arena_free(arena, NULL, 1);
    arena_free(arena, slots, 0);
    arena_free(arena, slots, -1);
    arena_destroy(NULL);
    arena_destroy(arena);
}

static void test_growth(void) {
    arena_t* arena = arena_create(MIB);
    ASSERT(arena != NULL);

    void* slots[65] = {0};
    ASSERT(arena_alloc(arena, slots, 65) == 65);
    for (int i = 0; i < 65; i++) {
        for (int j = 0; j < i; j++) {
            ASSERT(slots[i] != slots[j]);
        }
        uint8_t* bytes = (uint8_t*)slots[i];
        bytes[0] = (uint8_t)i;
        bytes[MIB - 1U] = (uint8_t)(i + 1);
        ASSERT(bytes[0] == (uint8_t)i);
        ASSERT(bytes[MIB - 1U] == (uint8_t)(i + 1));
    }

    arena_free(arena, slots, 65);
    arena_destroy(arena);
}

static void test_destroy_with_allocated_slot(void) {
    arena_t* arena = arena_create(4096);
    ASSERT(arena != NULL);

    void* slot = NULL;
    ASSERT(arena_alloc(arena, &slot, 1) == 1);
    ASSERT(slot != NULL);
    ((uint8_t*)slot)[0] = 0x5a;
    arena_destroy(arena);
}

int main(void) {
    test_create_limits();
    test_alloc_free_realloc();
    test_null_args();
    test_growth();
    test_destroy_with_allocated_slot();
    return 0;
}
