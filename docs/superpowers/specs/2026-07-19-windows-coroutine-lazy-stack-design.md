# Windows Coroutine Lazy Stack Design

## Summary

Xylem will keep arena-backed coroutine slots while adding native Windows x64
ASM stack growth. A Windows ASM slot reserves its full fixed stack but commits
only coroutine metadata, one moving guard page, and one initial usable stack
page. Windows grows the committed stack through its normal `PAGE_GUARD`
mechanism. Recycled hot slots return to the same initial page state so their
commit charge does not accumulate.

The implementation introduces a runtime `coro` adapter and a platform
`platform-coro` module. `coro` is the only layer that understands minicoro
layout and context details. `platform-coro` receives resolved memory ranges and
implements platform page policy. `copool`, `arena`, and `platform-vmem` remain
independent of minicoro internals.

## Goals

- Use the native Windows x64 moving `PAGE_GUARD` mechanism for ASM coroutine
  stacks without a process-wide VEH.
- Reserve the configured fixed stack size while charging only for the pages
  initially required by a new or recycled coroutine.
- Restore grown Windows ASM stacks to their initial committed state before a
  slot enters a hot cache.
- Keep local/shared copool hits free of VM system calls.
- Preserve current Windows Fiber and Unix-family behavior.
- Keep arena and virtual-memory modules generic.

## Non-Goals

- Dynamically resize or copy coroutine stacks.
- Change the externally configured coroutine stack size.
- Move Windows Fiber stacks into the arena.
- Add moving guards to Linux, Android, macOS, or iOS.
- Add a process-wide vectored exception handler.

## Ownership And Dependencies

The compile-time dependencies are:

```text
scheduler
    -> coro
        -> minicoro
        -> platform-coro
            -> platform-vmem
        -> copool
    -> copool
        -> arena
            -> platform-vmem
```

At runtime, `coro` supplies generic slot callbacks to `copool`. `copool` invokes
those callbacks without depending on `coro` or `platform-coro` types.

- `scheduler` owns the persistent minicoro descriptor template and `copool_t`.
- `coro` translates minicoro layout/context information into
  `platform_coro_t`.
- `platform-coro` manages the usable page layout of a coroutine slot.
- `copool` manages worker-local and shared hot-slot caches.
- `arena` owns regions, slot addresses, and the fully decommitted cold state.
- `platform-vmem` owns only generic reserve, commit, decommit, and release
  operations.

`arena` must not include or call `platform-coro` directly. A generic arena only
knows a slot address and size and must not depend on coroutine layout.

## Windows x64 ASM Slot Layout

The Windows x64 ASM minicoro layout reserves a page-aligned slot:

```text
low address

[coroutine metadata/context/storage]  committed PAGE_READWRITE
stack_low
[uncommitted stack reservation]
[moving guard page]                   PAGE_READWRITE | PAGE_GUARD
initial StackLimit
[initial usable stack page]           committed PAGE_READWRITE
StackBase

high address
```

The metadata span may occupy more than one page and ends at `stack_low`. The
embedded stack occupies `[stack_low, stack_low + stack_size)`. For the default
128 KiB stack and a 4 KiB metadata span, the slot is approximately 132 KiB
while its initial commit charge is approximately 12 KiB.

`stack_low` is the fixed lowest address of the embedded stack reservation. It
is not the Windows `StackLimit`. `StackLimit` is the current low boundary of the
ordinary read/write stack pages. The guard page is immediately below it. As the
stack grows downward, Windows moves the guard and decreases `StackLimit`.

The configured stack reservation remains:

```text
StackBase - stack_low == stack_size
```

The initial Windows context uses `StackBase = stack_low + stack_size` and
`DeallocationStack = stack_low`. The context switch continues saving and
restoring TEB `StackBase`, `StackLimit`, and `DeallocationStack`. Minicoro's
magic-number and stack-range check remains the delayed overflow fallback when
execution returns through `mco_yield()`.

## Platform Coroutine API

`src/platform/platform-coro.h` defines the resolved memory view passed by
`coro.c`:

```c
typedef struct platform_coro_s {
    void*  ptr;
    size_t size;
    void*  stack_low;
    size_t stack_size;
} platform_coro_t;

extern int platform_coro_init(const platform_coro_t* coro);

extern int platform_coro_reset(
    const platform_coro_t* coro,
    void* current_stack_limit);

extern void* platform_coro_initial_stack_limit(
    const platform_coro_t* coro);
```

`ptr` and `size` describe the complete page-aligned arena slot. `stack_low` and
`stack_size` describe only an embedded stack. An external-stack backend uses
`NULL` and zero.

All Windows ASM ranges must be page aligned and contained within the slot. The
nonempty metadata span ends at `stack_low`. Invalid layouts fail initialization
without entering a hot cache.

There is no `platform_coro_deinit()`. Returning a complete slot to the cold
state is a full-slot decommit owned by `arena_free()`. A platform-coro deinit
wrapper would duplicate that operation and invert the dependency from generic
arena code to coroutine-specific code.

### Windows x64 ASM

`platform_coro_init()` commits metadata, creates the initial guard page, and
commits the initial usable stack page. Unused stack pages remain uncommitted.

`platform_coro_initial_stack_limit()` returns the low address of the initial
ordinary read/write stack page, not the guard address.

`platform_coro_reset()` compares `current_stack_limit` with the calculated
initial value. Equal values take a no-system-call fast path. If the stack grew,
the function decommits the embedded stack and recreates the initial guard and
usable page. A `NULL` current value requests a complete reset and is used for a
partially initialized minicoro context.

Windows commit, decommit, and guard transitions apply the existing ASan shadow
poison/unpoison policy to the exact pages whose accessibility changes.

### Windows Fiber

Windows Fiber slots use `stack_low == NULL` and `stack_size == 0` because
`CreateFiberEx` owns the stack outside the arena slot. `init` commits the
metadata slot, `reset` is a no-op, and `initial_stack_limit` returns `NULL`.
`CreateFiberEx` and `DeleteFiber` behavior does not change.

### Linux, Android, macOS, And iOS

`init` makes the complete slot accessible and clears its ASan poison. `reset`
is a no-op so a hot slot keeps its current mapping. `initial_stack_limit`
returns `stack_low`; the corresponding minicoro setter is a no-op outside the
Windows x64 ASM backend.

## Minicoro Bridge

Minicoro exposes three layout/context helpers through its normal `MCO_API`
declaration mechanism:

```c
MCO_API size_t mco_desc_stack_offset(const mco_desc* desc);
MCO_API void* mco_get_stack_limit(const mco_coro* co);
MCO_API void mco_set_stack_limit(
    mco_coro* co,
    void* stack_limit);
```

The public functions have one shared implementation after minicoro selects its
backend. Backend sections only define internal compile-time capabilities:

- An external-stack backend makes `mco_desc_stack_offset()` return zero.
- Embedded-stack backends use the common tail-layout calculation.
- Windows x64 ASM enables access to `_mco_context.ctx.stack_limit`.
- Other backends make `get_stack_limit` return `NULL` and `set_stack_limit` a
  no-op.

`mco_get_stack_limit()` is null-safe for a missing or partially initialized
context. This is required because `mco_create()` invokes its deallocator after
an `mco_init()` failure.

The Windows ASM descriptor size calculation page-aligns the end of metadata
while preserving the requested stack capacity. Other backend descriptor
layouts remain unchanged.

## Runtime Coroutine Adapter

`src/runtime/coro.h` declares:

```c
extern mco_result coro_create(
    mco_coro** out,
    mco_desc* desc);

extern mco_result coro_destroy(mco_coro* co);

extern copool_slot_ops_t coro_get_slot_ops(
    const mco_desc* desc);
```

`coro_create()` calls `mco_create()`, builds a `platform_coro_t` from the
initialized coroutine, computes the initial platform stack limit, and writes
that address into the new minicoro context. `coro_destroy()` calls
`mco_destroy()`; the deallocation callback eventually invokes the copool reset
callback after `mco_uninit()` has preserved the saved context fields.

The slot callbacks are static functions in `coro.c`. Their `ud` points to the
scheduler-owned descriptor template and must remain valid until the copool is
destroyed.

## Copool Slot Lifecycle

`copool.h` defines:

```c
typedef struct copool_slot_ops_s {
    int (*init)(void* ptr, size_t size, void* ud);
    int (*reset)(void* ptr, size_t size, void* ud);
    void* ud;
} copool_slot_ops_t;

extern copool_t* copool_create(
    size_t slot_size,
    int32_t shared_cap,
    const copool_slot_ops_t* ops);
```

`copool_create()` copies the callback structure. Callback `size` is the actual
page-aligned arena slot size obtained from `arena_slot_size()` after arena
creation.

The lifecycle is:

```text
cold arena slot -> init  -> reusable hot slot
used slot       -> reset -> reusable hot slot
hot slot        -> arena_free -> cold arena slot
```

Local cache hits and local/shared transfers do not invoke callbacks. A refill
from arena calls `init` for every cold slot before caching it. Every used slot
calls `reset` before entering a local or shared cache. Slots spilled from a hot
cache have already been reset and go directly to the shared cache or arena.

An `init` or `reset` failure sends that slot to `arena_free()` instead of a hot
cache. Other slots in the same batch continue normally.

## Arena Cold-State Contract

The arena invariant is:

```text
Every address in free_slots represents a fully decommitted cold slot.
```

`_arena_grow()` reserves a complete region and decommits it once before adding
its addresses to `free_slots`. This normalizes Windows reservations and Unix
read/write mappings to the same logical cold state.

`arena_slot_size()` returns the page-aligned slot size selected by
`arena_create()`. This is the single source used by copool callbacks and avoids
duplicating alignment policy outside arena.

`arena_alloc()` only removes addresses from `free_slots`; it no longer commits
slots. `arena_free()` decommits each complete slot and only returns successful
slots to `free_slots`. If decommit fails, the slot is omitted from the free
list. Its containing region remains owned by the arena and is released by
`arena_destroy()`.

Arena destruction releases complete regions without individually resetting
hot slots. Scheduler teardown must continue ensuring no coroutine or pool
operation is in flight before destroying the arena.

## Scheduler Integration

`scheduler_t` replaces its duplicate `coro_stack_size` field with a persistent
descriptor template:

```c
mco_desc coro_desc;
copool_t* coro_pool;
```

Scheduler creation initializes `coro_desc`, installs the existing allocator
callbacks and scheduler allocator data, obtains `coro_get_slot_ops()`, and
creates the copool. Spawn copies the template, sets only per-coroutine
`user_data`, and calls `coro_create()`. Coroutine exit and shutdown cleanup call
`coro_destroy()`.

External configuration remains unchanged:

```text
xylem_opts.coro_stack_size
    -> runtime options
    -> scheduler options
    -> mco_desc_init
    -> scheduler.coro_desc.stack_size
```

The scheduler still decides when coroutines are created and destroyed. The new
adapter only centralizes how minicoro and platform stack state are initialized
and recycled.

## Failure Handling

- Region reserve or initial decommit failure releases the complete candidate
  region and fails that grow attempt.
- Platform coroutine init failure returns the slot through `arena_free()` and
  continues initializing other slots in the batch.
- If no slot in a refill can be initialized, `copool_alloc()` returns `NULL`.
- Platform coroutine reset failure bypasses all hot caches and returns the slot
  through `arena_free()`.
- Arena decommit failure logs an error and excludes the slot from `free_slots`.
- A missing current stack limit requests a complete Windows ASM reset.
- No normal hot-cache hit introduces a failure path or platform call.

## Verification

### Arena And Copool

- Arena cold allocation, return, reuse, region growth, and concurrent address
  uniqueness.
- Slot callback invocation counts across local, shared, and arena paths.
- Init and reset failure isolation.
- No duplicate callback calls during local/shared batch transfers.
- No VM callback on a local hot-cache allocation hit.

### Coroutine Adapter

- Embedded and external stack-offset reporting.
- Initial stack-limit installation after `coro_create()`.
- Current stack-limit forwarding during `coro_destroy()`.
- Safe reset after a partially initialized minicoro context.

### Windows x64 ASM

- Initial metadata, guard, and usable-page protections through `VirtualQuery`.
- Deep recursion and compiler `__chkstk` probes across multiple pages.
- Correct TEB `StackBase`, `StackLimit`, and `DeallocationStack` across context
  switches.
- Stack growth after coroutine migration between worker threads.
- No VM operation on reset when the stack did not grow.
- Commit-charge release and initial guard restoration after growth.
- Stack overflow handling when growth reaches the configured stack low bound.
- Hot/cold reuse under MSVC ASan.

### Other Backends

- Windows Fiber creation, execution, reuse, and destruction with an external
  stack.
- Linux and Apple-family whole-slot accessibility and hot reuse.
- Existing scheduler and runtime suites on the supported CI matrix.

### Performance

The existing single-worker and multi-worker spawn benchmarks remain the
performance baseline. The hot spawn path must not add a VM system call. Windows
cold-slot initialization may perform platform VM operations, amortized by the
existing copool batch refill policy.

## Files

New files:

- `src/runtime/coro.h`
- `src/runtime/coro.c`
- `src/platform/platform-coro.h`
- `src/platform/win/platform-coro.c`
- `src/platform/unix/platform-coro.c`
- `tests/test-coro.c`
- `tests/test-platform-coro.c`

Modified areas:

- minicoro layout and stack-limit bridge
- arena cold-state transitions
- copool lifecycle callbacks
- scheduler descriptor/lifecycle integration
- CMake source and test registration
- architecture, runtime, and platform design documentation
