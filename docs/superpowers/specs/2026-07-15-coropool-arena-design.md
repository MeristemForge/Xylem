# Coropool Arena Design

Status: Draft for written review
Date: 2026-07-15

## 1. Summary

Xylem will replace the scheduler's per-coroutine virtual-memory allocation
with a scheduler-wide coroutine memory pool implemented in
`src/runtime/coropool.c` and `src/runtime/coropool.h`.

The pool grows on demand by allocating arenas. Each arena contains at most 512
fixed-stride coroutine slots. A slot contains minicoro metadata, a 64-byte
stack canary, and the configured usable coroutine stack. The default usable
stack remains 128 KiB.

One `coropool_t` is shared by a scheduler. Each worker embeds a
`coropool_cache_t` holding up to 64 recycled slot addresses. Local-cache hits
are lock-free. A pool mutex protects the shared recycled array, arena list,
and arena bump allocation.

Arenas retain their virtual addresses and, on Windows, their commit charge
until the runtime is destroyed. Slot deallocation calls
`platform_vmem_reset()` so the operating system may discard the slot's
physical contents without releasing its address or Windows commit charge.

## 2. Motivation

The current scheduler allocates a separate virtual-memory mapping for every
new coroutine slot and protects one page as a stack guard page. On Linux and
WSL, each mapping and protection split consumes VMAs. The observed WSL limit
was about 32.7 thousand coroutines with the default `vm.max_map_count` of
about 65.5 thousand, even though address space and memory remained available.

Removing only the guard page would still leave approximately one mapping per
coroutine and would therefore retain an O(coroutines) VMA limit. Grouping up
to 512 slots in one arena changes mapping growth to O(coroutines / 512). For
100,000 slots, the pool needs about 196 arenas instead of tens of thousands of
per-slot mappings.

The arena design also removes per-slot guard-page protection calls and
amortizes virtual-memory allocation over hundreds of slots.

The fixed 128 KiB default is supported by measured Debug high-water marks in
the current TLS test paths:

| Test | Observed coroutine stack high-water |
| --- | ---: |
| TLS | 11,000 B |
| DTLS | 8,472 B |
| HTTPS | 21,144 B |
| WSS | 12,696 B |

These measurements are test-path observations, not a formal worst-case stack
bound. Applications can continue to configure a larger stack.

## 3. Goals

- Reduce coroutine-stack VMA usage from one or more VMAs per coroutine to
  approximately one VMA per arena.
- Preserve a configurable usable stack size with a 128 KiB default.
- Grow until an operating-system allocation fails; do not impose a configured
  coroutine-count limit.
- Keep coroutine creation and destruction lock-free on worker-local cache
  hits.
- Retain arena address space until scheduler destruction.
- Reset recycled slots so their physical contents may be discarded.
- Detect stack overflow with a canary plus minicoro's SP and metadata-magic
  checks, and abort immediately when detection occurs.
- Preserve a fallback for minicoro backends whose stacks are externally owned,
  such as `MCO_USE_FIBERS`.

## 4. Non-goals

- Grow or copy a coroutine stack while the coroutine is running.
- Release an empty arena before scheduler destruction.
- Guarantee immediate stack-overflow detection at the faulting instruction.
- Eliminate Windows system-wide commit charge for retained arenas.
- Add a public coropool API.
- Add per-worker arena ownership or remote-free queues.

## 5. Public Behavior

`xylem_opts_t.coro_stack_size` remains configurable. Zero selects the 128 KiB
default. A nonzero value denotes usable coroutine stack bytes and does not
include minicoro metadata or the 64-byte canary.

No public API changes are required. The internal
`scheduler_opts_t.coro_pool_capacity` field is removed because the new pool
tracks every arena slot and has no fixed shared-pool retention limit.

## 6. Module Boundary

The new files are:

```text
src/runtime/coropool.h
src/runtime/coropool.c
```

`coropool` owns:

- stack, slot, and arena size calculation;
- arena allocation, bump allocation, and destruction;
- the shared recycled-address array and its mutex;
- worker-local cache refill and drain operations;
- slot reset;
- canary initialization and validation.

The scheduler owns:

- worker TLS lookup;
- coroutine registry and scheduling state;
- selecting the current worker's cache, or `NULL` for an external thread;
- minicoro initialization, switching, and uninitialization;
- aborting on minicoro lifecycle errors.

The internal interface is:

```c
#define COROPOOL_CACHE_CAPACITY 64

typedef struct mco_coro mco_coro;
typedef struct coropool_s coropool_t;

typedef struct coropool_cache_s {
    void*   slots[COROPOOL_CACHE_CAPACITY];
    int32_t count;
} coropool_cache_t;

coropool_t* coropool_create(size_t stack_size);
void        coropool_destroy(coropool_t* pool);

size_t coropool_mco_stack_size(const coropool_t* pool);

void* coropool_alloc(
    coropool_t* pool,
    coropool_cache_t* cache);

void coropool_dealloc(
    coropool_t* pool,
    coropool_cache_t* cache,
    void* slot);

void coropool_arm(coropool_t* pool, mco_coro* co);
void coropool_check(coropool_t* pool, mco_coro* co);
```

`coropool_t` is opaque. `coropool_cache_t` is defined in the header because it
is embedded by value in each scheduler worker.

## 7. Slot Layout

The configured stack size is normalized through minicoro's existing minimum
and 16-byte alignment rules to obtain the effective usable stack size.
Coropool then asks minicoro to create a context with 64 additional bytes at
the low end of its downward-growing stack:

```text
page-aligned slot address, also the mco_coro address

+-------------------------------+
| minicoro metadata             |
+-------------------------------+
| 64-byte canary                | <- original co->stack_base
+-------------------------------+
| configured usable stack       | <- armed co->stack_base
| default: 128 KiB              |
+-------------------------------+ <- stack top, unchanged by arming
| page-alignment padding        |
+-------------------------------+
```

The descriptor passed to `mco_init()` uses:

```text
mco stack size = configured usable stack size + 64 bytes
```

The canary is eight repetitions of the fixed 64-bit value
`0xC0DEC0DEC0DEC0DE`. After successful `mco_init()`, `coropool_arm()` fills
the low 64 bytes with this pattern and then adjusts the public minicoro bounds:

```c
canary = co->stack_base;
fill_canary(canary, 64);

co->stack_base = (char*)co->stack_base + 64;
co->stack_size -= 64;
```

The stack top does not change, so the already-created minicoro context remains
valid. The exposed `co->stack_size` equals the configured usable size.

No per-slot prefix or descriptor is used. Free slot addresses are stored in
ordinary heap arrays and can remain there while the pointed-to slot contents
have been reset.

## 8. Size Calculation

Size calculation occurs once in `coropool_create()`.

```text
usable_desc     = mco_desc_init(dummy_entry, configured_stack_size)
usable_size     = usable_desc.stack_size
mco_stack_size  = checked_add(usable_size, 64)
layout_desc     = mco_desc_init(dummy_entry, mco_stack_size)
raw_slot_size   = layout_desc.coro_size
slot_stride     = align_up(raw_slot_size, page_size)

slots_per_arena = min(512, floor(128 MiB / slot_stride))
slots_per_arena = max(1, slots_per_arena)
arena_size      = checked_mul(slots_per_arena, slot_stride)
```

The 128 MiB value is a target, not a hard upper bound. If one configured slot
is larger than 128 MiB, an arena contains one slot and exceeds the target.

All additions, alignments, and multiplications use checked arithmetic. A zero
or invalid platform page size causes creation to fail.

With the current x86-64 minicoro metadata size and a 128 KiB usable stack:

| Base page size | Slot stride | 512-slot arena |
| ---: | ---: | ---: |
| 4 KiB | 132 KiB | 66 MiB |
| 8 KiB | 136 KiB | 68 MiB |
| 16 KiB | 144 KiB | 72 MiB |
| 32 KiB | 160 KiB | 80 MiB |
| 64 KiB | 192 KiB | 96 MiB |

The implementation queries `platform_vmem_page_size()` and does not hardcode
one of these sizes. Huge pages and Windows allocation granularity are not used
as the slot page size.

## 9. Pool and Arena State

The private structures are conceptually:

```c
typedef struct _coropool_arena_s {
    struct _coropool_arena_s* next;
    void*                     base;
    uint32_t                  bump;
    uint32_t                  capacity;
} _coropool_arena_t;

struct coropool_s {
    mtx_t lock;

    _coropool_arena_t* arenas;
    _coropool_arena_t* current;

    void** recycled;
    size_t recycled_count;
    size_t recycled_capacity;

    size_t stack_size;
    size_t mco_stack_size;
    size_t slot_stride;
    size_t slots_per_arena;
    size_t arena_size;
};
```

`bump` is the index of the next slot in an arena that has never been handed
out. It is not the live-coroutine count and never moves backwards. Recycled
slots are tracked separately.

When a new arena is added, the recycled-address array obtains enough capacity
to hold every slot created so far. This guarantees that later deallocation
does not need to allocate memory or discard a reusable address. Failure to
grow the tracking array prevents publication of the new arena.

Arena virtual memory is allocated lazily. `coropool_create()` creates pool
metadata but does not allocate the first arena.

## 10. Allocation Flow

The allocation order is:

1. Pop one address from the current worker's local cache without a lock.
2. If the local cache is empty, lock the pool and refill up to 32 addresses
   from the shared recycled array.
3. If no recycled address exists, take `current->bump` and advance it.
4. If no current arena has bump capacity, allocate and publish a new arena,
   then take its first slot.
5. Unlock the pool and return the page-aligned slot address.

An external thread passes `cache == NULL`, skips local-cache operations, and
uses the shared recycled or arena path directly. The initial root coroutine is
created this way because the thread running `runtime_run()` is not a scheduler
worker.

The pool uses `mtx_t`, not a spin lock, because arena growth performs
`platform_vmem_alloc()` while serializing publication. Worker-local hits do
not touch the mutex, and shared refill is batched.

## 11. Deallocation Flow

The scheduler performs the lifecycle in this order:

```text
coropool_check()
mco_uninit()
coropool_dealloc()
```

`coropool_dealloc()` resets the complete page-aligned slot with
`platform_vmem_reset(slot, slot_stride)` and then:

1. pushes the address into the current worker's local cache if it has room;
2. otherwise drains half of the 64-entry local cache into the shared recycled
   array under the pool mutex, then caches the newly freed slot;
3. or, when `cache == NULL`, pushes directly into the shared recycled array.

The shared recycled array has no configured retention limit. A slot is never
individually released because its arena owns the mapping until pool
destruction.

## 12. Virtual Memory Semantics

Coropool keeps the existing platform operations:

```text
platform_vmem_alloc()
platform_vmem_reset()
platform_vmem_dealloc()
```

It does not use a per-slot guard page and does not call
`platform_vmem_protect()`.

On Unix-like systems:

- one arena is one read/write anonymous `mmap`;
- physical pages are populated on demand;
- slot reset uses `madvise(MADV_DONTNEED)`;
- arena destruction uses `munmap`;
- reset does not change page protection and therefore does not create a
  persistent per-slot VMA split.

On Windows:

- arena allocation uses the current `MEM_RESERVE | MEM_COMMIT` operation;
- slot reset uses `MEM_RESET`;
- arena destruction uses `MEM_RELEASE`.

Windows commit charge is system-wide. `MEM_RESET` allows the system to discard
slot contents and physical backing but does not release commit charge. Commit
therefore follows the historical arena high-water mark and remains until
runtime destruction. With 4 KiB pages, 100,000 default slots require 196
arenas, or approximately 12.63 GiB of retained commit. This is an accepted
trade-off for avoiding per-coroutine commit/decommit operations.

## 13. Minicoro Lifecycle

The scheduler switches from allocator callbacks and `mco_create()` /
`mco_destroy()` to minicoro's explicit externally allocated lifecycle:

```text
create:
    coropool_alloc()
    mco_init()
    coropool_arm()

destroy:
    coropool_check()
    mco_uninit()
    coropool_dealloc()
```

If `mco_init()` fails, the scheduler calls `coropool_dealloc()` without arming
or checking the canary. This explicit failure path is why no per-slot armed
prefix is required.

The current allocator callbacks and their scheduler wrappers are removed.

For `MCO_USE_FIBERS` or another backend where minicoro or the operating system
owns a separate stack allocation, the scheduler bypasses coropool and keeps
the existing `mco_create()` / `mco_destroy()` external-stack path. It does not
claim to provide the arena canary for a stack it cannot address or own. The
arena and canary layout applies to the inline-stack `MCO_USE_ASM` and
`MCO_USE_UCONTEXT` paths. Windows x64 uses the inline assembly path; the
fallback primarily covers unsupported architectures and fiber-specific
builds.

## 14. Stack Overflow Handling

The 64-byte canary is initialized for every successfully initialized
coroutine. It is checked:

- immediately before scheduler-controlled `mco_yield()` calls;
- immediately after `mco_resume()` returns to a worker;
- before final coroutine uninitialization and slot reset.

A damaged canary causes an immediate `abort()`.

The scheduler also stops ignoring minicoro results. `MCO_STACK_OVERFLOW` from
the SP or metadata-magic check causes an immediate `abort()`. Unexpected
errors from `mco_yield()`, `mco_resume()`, or `mco_uninit()` are scheduler
invariant violations and also abort.

This mechanism detects overflow at scheduling boundaries, after memory may
already have been overwritten. It does not provide the immediate fault or
precise faulting instruction supplied by a guard page. A jump over the canary
may still be caught by minicoro's SP check if the stack pointer is out of
bounds at a checked yield. An out-of-bounds SP excursion that neither writes
the canary nor remains out of bounds at a check is not guaranteed to be
detected.

## 15. Concurrency and Lifetime

- A `coropool_cache_t` is accessed only by its owning worker.
- The pool mutex protects the shared recycled array, arena list, current arena,
  and bump indexes.
- A slot has no permanent worker owner. Work stealing does not require a
  remote-free queue.
- External callers use the shared path with a `NULL` cache.
- Scheduler shutdown joins workers and destroys or uninitializes registered
  coroutines before `coropool_destroy()` releases arenas.
- Local cache entries are borrowed addresses. They require no individual
  cleanup once all workers have stopped and the owning arenas are released.

## 16. Failure Handling

- Invalid or overflowing size calculation makes `coropool_create()` return
  `NULL`.
- Heap allocation, recycled-array growth, or arena allocation failure makes
  creation or allocation return `NULL` without publishing partial arena state.
- `scheduler_spawn()` propagates allocation or `mco_init()` failure through
  its existing failure result.
- `platform_vmem_reset()` remains a best-effort operation with its current
  void result. Failure to discard pages affects reclamation, not slot
  correctness.
- Canary damage and minicoro lifecycle invariant violations abort rather than
  allowing potentially corrupted execution to continue.

## 17. Rejected Alternatives

### Per-worker arenas

Per-worker arenas avoid a shared allocator lock but waste one partially used
arena per active worker and require remote-free handling after work stealing.
With 16 workers and one slot used by each, 512-slot arenas would commit about
1,056 MiB on Windows instead of one shared 66 MiB arena.

### Per-slot descriptor objects

External slot descriptors are unnecessary. Worker caches and the shared
recycled array store raw slot address values in ordinary heap memory, so they
remain readable even after slot contents are reset.

### Intrusive free list inside slots

An intrusive list would make free-list state depend on reset slot contents.
The external recycled-address array is simpler and preserves the option to
change reset semantics later.

### Per-slot reserve and commit phases

Committing on every allocation and decommitting on every free would preserve
commit more precisely but would add operating-system calls to coroutine churn
and make the local cache ineffective. The selected design commits an arena
once and resets slots without decommitting them.

### Per-slot guard pages

Guard pages provide immediate overflow faults but recreate the VMA scaling
problem that motivates this change.

### Arena release during runtime

Releasing empty arenas requires per-arena live accounting and removal of every
cached address that points into the arena. The selected design retains arenas
until scheduler destruction.

## 18. Verification

Focused coropool and scheduler tests will cover:

- default and custom usable stack sizes;
- checked layout calculation and page-aligned slot addresses;
- 513 allocations creating a second default arena;
- unique addresses for simultaneously allocated slots;
- recycled-address reuse across multiple arenas;
- local-cache refill and half-drain behavior;
- `cache == NULL` allocation and deallocation;
- concurrent allocation and deallocation with one cache per worker thread;
- repeated runtime creation and destruction;
- `mco_init()` failure cleanup;
- canary corruption causing process abort;
- `MCO_STACK_OVERFLOW` causing process abort;
- ASAN and UBSAN runs where supported;
- the full existing test suite, including TLS, DTLS, HTTPS, and WSS.

On Linux or WSL, a stress diagnostic will additionally confirm that coroutine
count no longer approaches `vm.max_map_count` linearly and that arena growth
is visible as approximately one mapping per arena rather than per slot.

## 19. Acceptance Criteria

- Existing public behavior and tests remain compatible.
- The default usable coroutine stack is 128 KiB, excluding the canary and
  metadata.
- More than 32.7 thousand default-stack coroutines can be created on the
  previously limited WSL environment when memory resources permit.
- A 100,000-coroutine stress run is not limited by VMA count.
- Local-cache allocation and deallocation do not acquire the pool mutex.
- No per-slot guard-page protection remains in the inline-stack path.
- Arena addresses and Windows commit are released at runtime destruction.
- Canary or minicoro overflow detection terminates the process immediately.
