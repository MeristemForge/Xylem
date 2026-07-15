# Coropool Arena Design

Status: Draft for written review
Date: 2026-07-15
Updated: 2026-07-16

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

Arenas retain their virtual addresses until the runtime is destroyed. On
Windows, an arena is reserved without being committed. Active slots and
worker-local cached slots are committed; slots moved to the shared recycled
array are decommitted. This bounds idle commit while preserving a syscall-free
local allocation path.

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

The current Windows pool releases individual mappings once the bounded local
and shared caches are full, so commit falls after a concurrency spike. A naive
whole-arena commit policy would regress that behavior by retaining the peak
until runtime destruction. The selected hybrid policy retains arena addresses
but decommits slots spilled beyond worker-local caches.

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
- Reset worker-local cached slots so their physical contents may be discarded.
- Decommit shared recycled slots so Windows commit falls after a concurrency
  spike.
- Detect stack overflow with a canary plus minicoro's SP and metadata-magic
  checks, and abort immediately when detection occurs.
- Preserve a fallback for minicoro backends whose stacks are externally owned,
  such as `MCO_USE_FIBERS`.

## 4. Non-goals

- Grow or copy a coroutine stack while the coroutine is running.
- Release an empty arena before scheduler destruction.
- Guarantee immediate stack-overflow detection at the faulting instruction.
- Eliminate Windows commit charge for active stacks or the bounded
  worker-local committed caches.
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
- slot commit, reset, and decommit transitions;
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

Arena virtual memory is reserved lazily. `coropool_create()` creates pool
metadata but does not reserve the first arena.

Slot location defines its normal backing state:

| Location | Windows state | Unix state |
| --- | --- | --- |
| Arena at or beyond `bump` | reserved only | mapped, untouched |
| Active coroutine | committed | mapped, populated on demand |
| Worker-local cache | committed and reset | mapped and reset |
| Shared recycled array | decommitted | mapped and discarded |

The shared recycled array contains addresses, not data stored inside the
slots, so decommitting a shared slot does not make pool metadata inaccessible.

## 10. Allocation Flow

The allocation order is:

1. Pop one committed address from the current worker's local cache without a
   lock or virtual-memory call.
2. If the local cache is empty, remove up to 32 decommitted addresses from the
   shared recycled array under the pool mutex.
3. Commit the removed addresses outside the mutex. Keep successful commits in
   the local cache and return one of them. Return failed commits to the shared
   recycled array before reporting allocation failure.
4. If no recycled address exists, claim `current->bump` and advance it under
   the mutex.
5. If no current arena has bump capacity, reserve and publish a new arena,
   then claim its first slot.
6. Commit a newly claimed bump slot before returning it. If commit fails, put
   the still-reserved address into the shared recycled array and return
   `NULL`.

An external thread passes `cache == NULL`, skips local-cache operations, and
commits only the one shared or bump slot it will return. The initial root
coroutine is created this way because the thread running `runtime_run()` is
not a scheduler worker.

The pool uses `mtx_t`, not a spin lock, because arena growth performs
`platform_vmem_reserve()` while serializing publication. Worker-local hits do
not touch the mutex, and shared refill is batched. Slot commit calls occur
outside the mutex after the corresponding addresses have been removed from
shared ownership.

## 11. Deallocation Flow

The scheduler performs the lifecycle in this order:

```text
coropool_check()
mco_uninit()
coropool_dealloc()
```

`coropool_dealloc()` then applies the following policy:

1. If the current worker's local cache has room, reset the complete
   page-aligned slot with `platform_vmem_reset(slot, slot_stride)` and cache
   its still-committed address without taking the pool mutex.
2. If the local cache is full, remove half of its 64 entries. Decommit those
   entries outside the pool mutex, append their addresses to the shared
   recycled array under the mutex, then reset and locally cache the newly
   freed slot.
3. When `cache == NULL`, decommit the newly freed slot and append its address
   directly to the shared recycled array.

The shared recycled array has no configured retention limit. A slot is never
individually released because its arena owns the reservation until pool
destruction. A decommit failure affects commit reclamation but not address
ownership. The address still enters the shared array; recommitting an already
committed Windows range is valid and idempotent.

## 12. Virtual Memory Semantics

Coropool uses an explicit backing lifecycle:

```c
void* platform_vmem_reserve(size_t size);
int   platform_vmem_commit(void* ptr, size_t size);
void  platform_vmem_reset(void* ptr, size_t size);
int   platform_vmem_decommit(void* ptr, size_t size);
void  platform_vmem_release(void* ptr, size_t size);
```

It does not use a per-slot guard page and does not call
`platform_vmem_protect()`.

On Unix-like systems:

- reserve creates one read/write anonymous `mmap` for the complete arena;
- commit is a logical operation and does not call `mprotect`;
- physical pages are populated on demand;
- reset and decommit use `madvise(MADV_DONTNEED)`;
- release uses `munmap`;
- reset does not change page protection and therefore does not create a
  persistent per-slot VMA split.

On Windows:

- reserve uses `VirtualAlloc(..., MEM_RESERVE, PAGE_NOACCESS)` for the complete
  arena;
- commit uses `VirtualAlloc(slot, slot_stride, MEM_COMMIT, PAGE_READWRITE)`;
- slot reset uses `MEM_RESET`;
- decommit uses `VirtualFree(slot, slot_stride, MEM_DECOMMIT)`;
- release uses `VirtualFree(arena, 0, MEM_RELEASE)`.

Windows commit charge is system-wide. `MEM_RESET` allows the system to discard
local-cache contents and physical backing but does not release commit charge.
`MEM_DECOMMIT` releases the commit charge of slots moved to shared recycled
storage while preserving the encompassing arena reservation.

At a concurrency peak, active fixed stacks still require their full committed
slot sizes. With 4 KiB pages, 100,000 active default slots require about
12.63 GiB of commit. After those coroutines finish, the normal retained idle
commit is bounded by worker-local caches:

```text
worker_count * COROPOOL_CACHE_CAPACITY * slot_stride
```

For 16 workers, 64 cached slots per worker, and a 132 KiB stride, that bound is
approximately 132 MiB. Shared recycled slots and never-used bump slots do not
retain Windows commit. Arena address space is released only at runtime
destruction.

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
- Heap allocation, recycled-array growth, arena reserve, or slot commit
  failure makes creation or allocation return `NULL` without losing the slot
  address or publishing partial arena state.
- `scheduler_spawn()` propagates allocation or `mco_init()` failure through
  its existing failure result.
- `platform_vmem_reset()` remains a best-effort operation with its current
  void result. Failure to discard pages affects reclamation, not slot
  correctness.
- `platform_vmem_decommit()` failure leaves excess commit charged but does not
  invalidate the reserved address. A later commit of that shared address is
  idempotent.
- Canary damage and minicoro lifecycle invariant violations abort rather than
  allowing potentially corrupted execution to continue.

## 17. Rejected Alternatives

### Per-worker arenas

Per-worker arenas avoid a shared allocator lock but waste one partially used
arena per active worker and require remote-free handling after work stealing.
With 16 workers and one slot used by each, 512-slot arenas would reserve about
1,056 MiB of fragmented address space instead of sharing one 66 MiB arena.
More importantly, free capacity owned by one worker would not automatically
satisfy demand on another worker.

### Per-slot descriptor objects

External slot descriptors are unnecessary. Worker caches and the shared
recycled array store raw slot address values in ordinary heap memory, so they
remain readable even after slot contents are reset.

### Intrusive free list inside slots

An intrusive list would make free-list state depend on reset slot contents.
The external recycled-address array is simpler and preserves the option to
change reset semantics later.

### Whole-arena commit

Using `MEM_RESERVE | MEM_COMMIT` once per arena minimizes Windows VM calls but
retains the historical concurrency high-water as system-wide commit. A burst
to 100,000 default slots would leave about 12.63 GiB charged until runtime
destruction even after all connections closed. The selected design reserves
the arena and commits individual slots instead.

### Decommit on every coroutine destruction

Immediately decommitting every freed slot would preserve commit most precisely
but would add a decommit and recommit to every hot coroutine lifecycle. The
selected design keeps the bounded worker-local caches committed and decommits
only slots spilled to shared recycled storage.

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
- local committed, shared decommitted, and bump reserved-only state
  transitions;
- `cache == NULL` allocation and deallocation;
- concurrent allocation and deallocation with one cache per worker thread;
- repeated runtime creation and destruction;
- `mco_init()` failure cleanup;
- slot commit failure returning its address to shared ownership;
- decommit failure preserving a reusable address;
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
- Shared recycled slots release Windows commit during runtime operation.
- Normal idle Windows commit is bounded by worker-local cache capacity.
- Arena addresses and any remaining Windows commit are released at runtime
  destruction.
- Canary or minicoro overflow detection terminates the process immediately.
