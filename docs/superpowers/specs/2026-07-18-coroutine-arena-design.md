# Coroutine Arena Design

## Scope

Replace per-coroutine virtual-memory mappings with one scheduler-owned arena.
Every minicoro allocator callback obtains its memory from the arena. The
existing per-worker and shared coroutine pools remain as committed caches in
front of the arena.

The arena owns virtual address ranges and fixed-size slots. It does not own
coroutine lifecycle state, runnable queues, worker assignment, or minicoro
context initialization.

## Goals

- Keep coroutine spawn and exit lock-free on worker-local cache hits.
- Amortize shared-pool and arena synchronization through batches of 32 slots.
- Bound each virtual-memory region to 64 MiB while guaranteeing at least 64
  slots per region.
- Avoid one virtual-memory mapping per coroutine.
- Keep local and shared cache slots committed for fast reuse.
- Decommit only cold slots returned to the arena.
- Support Linux, Android, macOS, iOS, and Windows through `platform-vmem`.
- Keep the arena API independent from scheduler and minicoro types.

## Ownership and Lifetime

The scheduler owns one `arena_t`:

```text
scheduler_create
  -> calculate mco_desc.coro_size
  -> arena_create(coro_size)
  -> reserve the first region immediately

scheduler_destroy
  -> stop and join workers
  -> destroy remaining coroutines
  -> discard local/shared pointer caches
  -> arena_destroy
```

Regions remain reserved for the scheduler lifetime. A completely free region
is not released while the scheduler is running. Cold slots can release physical
resources through decommit while retaining their addresses. All regions are
released together by `arena_destroy()`.

## Public Internal API

The module lives in `src/runtime/arena.h` and `src/runtime/arena.c`.

```c
typedef struct arena_s arena_t;

extern arena_t* arena_create(size_t slot_size);
extern void arena_destroy(arena_t* arena);

extern int arena_alloc(
    arena_t* arena,
    void**   slots,
    int      count);

extern void arena_free(
    arena_t* arena,
    void**   slots,
    int      count);
```

`arena_alloc()` returns the number of successfully committed slots written to
`slots`, from zero through `count`. Partial success is valid. `arena_free()` is
void because a decommit failure does not prevent the arena from recovering slot
ownership.

## Fixed Slot Layout

One arena supports one fixed slot size. `arena_create()` page-aligns the raw
`mco_desc.coro_size` supplied by the scheduler. Every slot begins at a page
boundary and occupies exactly the aligned size.

There is no per-slot header or other metadata inside the slot. A cold slot may
be inaccessible on Windows and is poisoned in ASan builds after a successful
decommit. Arena metadata must therefore remain outside slot memory.

Changing the configured coroutine stack size requires creating a new scheduler
and arena.

## Data Structures

```c
#define ARENA_REGION_MIN_SLOTS 64
#define ARENA_REGION_MAX_SIZE  (64 * 1024 * 1024)

typedef struct _arena_region_s {
    struct _arena_region_s* next;
    void*                   base;
    size_t                  size;
    size_t                  slot_count;
} _arena_region_t;

struct arena_s {
    mtx_t            lock;
    _arena_region_t* regions;
    void**            free_slots;
    size_t            free_count;
    size_t            free_cap;
    size_t            slot_size;
};
```

`free_slots` is an external LIFO stack of addresses. It is used instead of an
intrusive list because decommitted slot contents cannot hold a reliable `next`
pointer. No per-slot bitmap or descriptor is required while regions have a
single slot size and remain reserved until destroy.

## Region Sizing

After page alignment, the slot size must satisfy:

```text
slot_size * ARENA_REGION_MIN_SLOTS <= ARENA_REGION_MAX_SIZE
```

The effective maximum slot size is therefore 1 MiB. This check applies to the
complete aligned minicoro allocation, not only the configured stack size.

Each region-growth attempt begins with:

```c
size_t slot_count = ARENA_REGION_MAX_SIZE / arena->slot_size;
```

If reserve fails, `slot_count` is halved until the 64-slot minimum is attempted:

```text
maximum fitting slot count
half
quarter
...
64
```

Every attempted region size is an exact multiple of `slot_size`. Different
regions may have different sizes after fallback. If the 64-slot attempt fails,
growth fails.

The initial region is created eagerly by `arena_create()`. Later regions are
created only when an allocation request cannot be satisfied from
`free_slots`.

## Region Growth

Region growth runs while holding `arena->lock`:

1. Reserve a region using the fallback sequence.
2. Allocate a region descriptor.
3. Grow `free_slots` so its capacity includes every slot in the new region.
4. Link the region into `regions`.
5. Append `base + i * slot_size` for every slot.

If descriptor allocation or `free_slots` growth fails, the new reservation is
released and the growth attempt fails.

Fresh regions are not decommitted. Their slots have never escaped the arena,
and the first allocation still passes through `platform_vmem_commit()`. This
avoids an unnecessary whole-region advice call on Linux and Darwin. Windows
reservations are already uncommitted.

One allocation call grows at most one region. Scheduler requests are bounded by
the 32-slot batch size, while every region contains at least 64 slots.

## Allocation

`arena_alloc()` performs ownership selection under the mutex and page-state
transitions outside it:

```text
lock
  grow one region when free_count < requested count
  pop up to requested count addresses
unlock

commit each selected slot
  compact successful addresses at the start of the output array
  immediately return each failed address to free_slots under the mutex

return successful count
```

A commit failure does not trigger another region growth in the same call. The
caller uses any partial result and treats zero as allocation failure. Commit
failure is an exceptional path, so re-locking once per failed slot is simpler
than allocating a temporary failure array and does not affect successful batch
allocation.

## Freeing

`arena_free()` decommits slots without holding the arena mutex:

```text
for each slot
  platform_vmem_decommit(slot, slot_size)
  log an error if decommit fails

lock
  append all slot addresses to free_slots
unlock
```

A slot is returned even when decommit fails. Its pages may remain committed,
but the next allocation still performs the idempotent commit transition. This
preserves allocator ownership and avoids leaking a slot because a physical-page
reclaim hint failed.

`free_count + count` must not exceed `free_cap`. Exceeding the capacity means
the allocator received a duplicate or foreign pointer, or its lifetime rules
were violated. The arena logs a fatal diagnostic and aborts.

## Synchronization

The arena uses one plain C11 `mtx_t`. It protects the region list, free-slot
stack, capacity changes, and region growth. Reserve and metadata allocation may
run while this mutex is held because growth is a cold path and concurrent
growers would otherwise reserve redundant regions.

`platform_vmem_commit()` and `platform_vmem_decommit()` run without the arena
mutex. No shared-pool spinlock may be held while calling an arena function.

This follows the same broad strategy as Go's stack allocator: per-P cache hits
avoid locks, while global stack-pool refill and span allocation use locks. A
lock-free arena bitmap or tagged atomic free list is not justified on the cold
path and can be introduced later without changing the arena API.

## Scheduler Cache Integration

The shared scheduler pool contains only committed slots and uses the arena as
its backing allocator:

```c
typedef struct _sched_coro_pool_s {
    spin_t   lock;
    void**   slots;
    int32_t  count;
    int32_t  cap;
    arena_t* arena;
} _sched_coro_pool_t;
```

Worker allocation order:

```text
local cache
shared pool, up to 32 slots
arena_alloc, up to 32 slots
```

The selected slot is returned to minicoro and remaining slots fill the worker's
local cache.

Non-worker allocation order:

```text
shared pool, one slot
arena_alloc, up to 32 slots
```

On an arena batch allocation, one slot is returned and the remainder are placed
in the shared pool. Any shared-pool overflow is returned to the arena. The
shared spinlock is released before either arena call, so the two locks are never
nested. Concurrent non-worker refills may reserve multiple batches; overflow
handling keeps the result correct without a separate refill state.

Worker free order:

```text
local cache when it has room
otherwise retain half locally
move as many excess slots as possible to the shared pool
return shared overflow to the arena in one batch
place the newly freed slot in the local cache
```

Non-worker frees enter the shared pool when it has capacity and otherwise
return directly to the arena.

Local and shared cache hits do not call commit or decommit. Only slots crossing
the arena boundary perform platform virtual-memory transitions.

## Minicoro Backend Boundary

Every allocation made through minicoro's allocator callback uses the arena.

For ASM and ucontext backends, `mco_desc.coro_size` includes the coroutine
object, context, storage, and stack, so the complete allocation is inside the
arena slot.

On Windows targets that use `MCO_USE_FIBERS`, the arena slot contains the
coroutine object, context, and storage. The stack remains owned by
`CreateFiberEx()` and `DeleteFiber()` because the Windows Fiber API does not
accept a caller-provided stack address.

The scheduler no longer uses a separate calloc/free allocator path for Fiber
mode.

## Scheduler Cleanup

Scheduler cleanup must preserve this order:

1. Stop and join workers.
2. Destroy every remaining coroutine while allocator callbacks are valid.
3. Free worker-local cache pointer arrays.
4. Free the shared-pool pointer array.
5. Destroy the arena and release every region.

Local and shared slot addresses are not individually released. They refer to
memory owned by arena regions.

## Input and Error Semantics

- `arena_create(0)` returns `NULL`.
- Page-alignment arithmetic checks `size_t` overflow.
- An aligned slot size above 1 MiB returns `NULL`.
- `arena_alloc(NULL, ...)`, a NULL output array, or `count <= 0` returns zero.
- `arena_free(NULL, ...)`, a NULL input array, or `count <= 0` is a no-op.
- A minicoro allocation request larger than the arena slot size logs an error
  and returns `NULL`.
- A failed initial region reservation makes `arena_create()` fail.
- A failed later region growth permits `arena_alloc()` to return slots that
  were already free, otherwise it returns zero.
- Region release failures during destroy are logged while cleanup continues.

## Removed Scheduler Responsibilities

The scheduler no longer calculates page-rounded coroutine metadata or stack
subranges. It no longer commits, decommits, or releases individual coroutine
allocations. These responsibilities move behind `arena_alloc()`, `arena_free()`,
and `arena_destroy()`.

## Verification

Add `tests/test-arena.c` with coverage for:

- zero and oversized slot rejection;
- the exact 1 MiB slot boundary;
- page-aligned, distinct, writable batch allocations;
- free followed by recommit without assuming preserved contents;
- region growth after exhausting available slots;
- concurrent batch allocation and free with a test-side active-pointer set;
- destroy with both allocated and free slots;
- ASan reuse without stale poison reports.

Scheduler tests cover:

- worker-local hits;
- half-cache shared refill;
- shared miss followed by a 32-slot arena refill;
- local/shared overflow returned to the arena;
- non-worker batch refill into the shared pool;
- concurrent coroutine creation and destruction;
- scheduler creation failure when the final aligned slot exceeds 1 MiB.

Update the runtime, platform, and architecture design documents to describe
region ownership, committed caches, platform decommit behavior, and region-level
VMA growth.

On Linux, perform a targeted integration check with many simultaneously alive
coroutines and `/proc/self/maps`. Mapping count should grow with the number of
arena regions rather than the number of coroutine slots. Keep this check out of
the portable unit suite because ASan, shared libraries, and thread runtimes add
unrelated mappings.
