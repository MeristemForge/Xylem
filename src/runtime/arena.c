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

#include "arena.h"

#include "xylem/xylem-logger.h"
#include "xylem/xylem-threads.h"

#include "platform/platform-vmem.h"

#include <stdint.h>
#include <stdlib.h>

#define ARENA_REGION_MIN_SLOTS 64U
#define ARENA_REGION_MAX_SIZE  (64U * 1024U * 1024U)
#define ARENA_SLOT_MAX_SIZE    (1024U * 1024U)

typedef struct _arena_region_s _arena_region_t;

struct _arena_region_s {
    _arena_region_t* next;
    void*            base;
    size_t           size;
    size_t           slot_count;
};

struct arena_s {
    mtx_t            lock;
    _arena_region_t* regions;
    void**           free_slots;
    size_t           free_count;
    size_t           free_cap;
    size_t           slot_size;
};

static int _arena_release_region(void* ptr, size_t size) {
    VMEM_ASAN_UNPOISON(ptr, size);
    if (platform_vmem_release(ptr, size) != 0) {
        VMEM_ASAN_POISON(ptr, size);
        return -1;
    }
    return 0;
}

static int _arena_grow(arena_t* arena) {
    size_t slot_count = ARENA_REGION_MAX_SIZE / arena->slot_size;
    size_t region_size;
    void*  base;

    for (;;) {
        region_size = slot_count * arena->slot_size;
        base        = platform_vmem_reserve(region_size);
        if (base) {
            break;
        }
        if (slot_count == ARENA_REGION_MIN_SLOTS) {
            return -1;
        }

        slot_count /= 2;
        if (slot_count < ARENA_REGION_MIN_SLOTS) {
            slot_count = ARENA_REGION_MIN_SLOTS;
        }
    }
    if (platform_vmem_decommit(base, region_size) != 0) {
        (void)_arena_release_region(base, region_size);
        return -1;
    }
    VMEM_ASAN_POISON(base, region_size);

    _arena_region_t* region =
        (_arena_region_t*)calloc(1, sizeof(_arena_region_t));
    if (!region) {
        (void)_arena_release_region(base, region_size);
        return -1;
    }

    if (slot_count > SIZE_MAX - arena->free_cap ||
        arena->free_cap + slot_count > SIZE_MAX / sizeof(void*)) {
        free(region);
        (void)_arena_release_region(base, region_size);
        return -1;
    }

    size_t new_cap = arena->free_cap + slot_count;
    void** free_slots =
        (void**)realloc(arena->free_slots, new_cap * sizeof(void*));
    if (!free_slots) {
        free(region);
        (void)_arena_release_region(base, region_size);
        return -1;
    }

    region->next       = arena->regions;
    region->base       = base;
    region->size       = region_size;
    region->slot_count = slot_count;

    arena->free_slots = free_slots;
    arena->free_cap   = new_cap;
    arena->regions    = region;

    uint8_t* slot = (uint8_t*)base;
    for (size_t i = 0; i < slot_count; i++) {
        arena->free_slots[arena->free_count + i] = slot;
        slot += arena->slot_size;
    }
    arena->free_count += slot_count;
    return 0;
}

arena_t* arena_create(size_t slot_size) {
    size_t page_size = platform_vmem_page_size();
    if (slot_size == 0 || page_size == 0 ||
        slot_size > SIZE_MAX - (page_size - 1)) {
        return NULL;
    }

    slot_size = ((slot_size + page_size - 1) / page_size) * page_size;
    if (slot_size > ARENA_SLOT_MAX_SIZE) {
        return NULL;
    }

    arena_t* arena = (arena_t*)calloc(1, sizeof(arena_t));
    if (!arena) {
        return NULL;
    }
    if (mtx_init(&arena->lock, mtx_plain) != thrd_success) {
        free(arena);
        return NULL;
    }
    arena->slot_size = slot_size;

    mtx_lock(&arena->lock);
    int rc = _arena_grow(arena);
    mtx_unlock(&arena->lock);
    if (rc != 0) {
        mtx_destroy(&arena->lock);
        free(arena);
        return NULL;
    }
    return arena;
}

void arena_destroy(arena_t* arena) {
    if (!arena) {
        return;
    }

    _arena_region_t* region = arena->regions;
    while (region) {
        _arena_region_t* next = region->next;
        if (_arena_release_region(region->base, region->size) != 0) {
            xylem_loge("<arena> release failed ptr=%p", region->base);
        }
        free(region);
        region = next;
    }
    free(arena->free_slots);
    mtx_destroy(&arena->lock);
    free(arena);
}

size_t arena_slot_size(const arena_t* arena) {
    return arena ? arena->slot_size : 0;
}

int arena_alloc(arena_t* arena, void** slots, int count) {
    if (!arena || !slots || count <= 0) {
        return 0;
    }

    mtx_lock(&arena->lock);
    if (arena->free_count < (size_t)count) {
        (void)_arena_grow(arena);
    }
    size_t take_count = arena->free_count;
    if (take_count > (size_t)count) {
        take_count = (size_t)count;
    }
    for (size_t i = 0; i < take_count; i++) {
        slots[i] = arena->free_slots[--arena->free_count];
        VMEM_ASAN_UNPOISON(slots[i], arena->slot_size);
    }
    mtx_unlock(&arena->lock);
    return (int)take_count;
}

void arena_free(arena_t* arena, void** slots, int count) {
    if (!arena || !slots || count <= 0) {
        return;
    }

    int cold_count = 0;
    for (int i = 0; i < count; i++) {
        void* slot = slots[i];
        if (platform_vmem_decommit(slot, arena->slot_size) == 0) {
            VMEM_ASAN_POISON(slot, arena->slot_size);
            slots[cold_count++] = slot;
        } else {
            xylem_loge("<arena> decommit failed ptr=%p", slot);
        }
    }
    if (cold_count == 0) {
        return;
    }

    mtx_lock(&arena->lock);
    if ((size_t)cold_count > arena->free_cap - arena->free_count) {
        xylem_loge("<arena> free slot overflow");
        abort();
    }
    for (int i = 0; i < cold_count; i++) {
        arena->free_slots[arena->free_count++] = slots[i];
    }
    mtx_unlock(&arena->lock);
}
