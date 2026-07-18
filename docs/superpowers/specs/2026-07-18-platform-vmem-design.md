# Platform Virtual Memory Lifecycle Design

## Scope

Refactor the internal `platform-vmem` API to separate virtual address
reservation from physical-memory commitment. This is the platform foundation
for the coroutine arena, but this change does not add the arena itself.

The refactor replaces the current combined allocation/reset interface on
Linux, Android, macOS, iOS, and Windows. Existing scheduler behavior remains
supported until the arena migration is implemented.

## API

Keep the existing page-size query and expose four lifecycle transitions:

```c
extern size_t platform_vmem_page_size(void);

extern void* platform_vmem_reserve(size_t size);

extern int platform_vmem_commit(
    void* ptr,
    size_t size);

extern int platform_vmem_decommit(
    void* ptr,
    size_t size);

extern int platform_vmem_release(
    void* ptr,
    size_t size);
```

Remove the following interfaces after migrating their scheduler callers:

- `platform_vmem_alloc()`
- `platform_vmem_reset()`
- `platform_vmem_dealloc()`

Keep `platform_vmem_protect()` during this refactor because the existing
scheduler still installs one guard page per coroutine allocation. The later
coroutine arena change removes the guard page and then removes the protection
API when it has no callers.

## Common Semantics

`reserve()` acquires one contiguous virtual address range. The returned base
and the size passed to all other functions must be system-page aligned.

`commit()` makes a reserved range available for read/write access. It is
idempotent. It must also clear stale ASAN poison before returning the range to a
caller.

`decommit()` discards the range contents while preserving the containing
reservation. The caller must not access the range again until `commit()`
succeeds, even on systems where the address technically remains accessible.
ASAN builds poison the range after it is decommitted.

`release()` returns a complete reservation to the operating system. Partial
region release is not supported.

Status-returning functions use `0` for success and `-1` for failure. A failed
`decommit()` means physical backing may not have been discarded; the
reservation remains valid and may be recommitted and recycled. A failed
`commit()` must not publish the slot to the scheduler.

The API does not promise that recommitted memory retains prior contents.

## Platform Behavior

### Linux and Android

- `reserve`: one read/write anonymous private `mmap` for the complete region.
- `commit`: no mapping transition; clear ASAN poison and return success.
- `decommit`: `madvise(MADV_DONTNEED)`.
- `release`: `munmap` the complete region.

The region is mapped read/write once so per-slot commit and decommit operations
do not use `mprotect` and do not split the region into additional VMAs.

After reserving the complete region, attempt `madvise(MADV_NOHUGEPAGE)` on the
whole mapping when the constant is available. Coroutine slots are independently
discarded and are a poor fit for transparent huge pages. This call is a hint;
an unsupported or rejected hint does not fail the reservation. Applying it to
the complete mapping avoids slot-granularity VMA splits.

Do not use `MAP_NORESERVE`. Reservation failure remains observable and can be
handled by the arena's region-size fallback instead of surfacing as a later
fatal page fault.

### macOS and iOS

- `reserve`: one read/write anonymous private `mmap` for the complete region.
- `commit`: `madvise(MADV_FREE_REUSE)` and clear ASAN poison.
- `decommit`: poison under ASAN, then `madvise(MADV_FREE_REUSABLE)`.
- `release`: `munmap` the complete region.

The Darwin reusable advice pair keeps kernel task accounting accurate while
allowing discarded pages to be reclaimed. No per-slot protection transition is
performed.

### Windows

- `reserve`: `VirtualAlloc(..., MEM_RESERVE, PAGE_NOACCESS)`.
- `commit`: `VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE)` and clear
  ASAN poison.
- `decommit`: poison under ASAN, then `VirtualFree(ptr, size, MEM_DECOMMIT)`.
- `release`: `VirtualFree(ptr, 0, MEM_RELEASE)`.

Unused slots consume address space but no system commit charge. Only committed
slots are readable and writable.

## Deliberately Omitted Operations

The Go runtime exposes a larger state machine because its vmem layer supports a
general heap allocator. Xylem's fixed-slot arena does not need equivalents for:

- `sysAlloc`: callers use `reserve()` followed by `commit()`.
- `sysMap`: Unix regions are mapped read/write once; Windows commits slots
  directly.
- `sysFault`: runtime debug fault mappings are out of scope. The existing
  protection API remains temporarily for the scheduler's guard page.
- `sysHugePage`: coroutine regions instead disable transparent huge pages on
  Linux and Android.
- aligned reservation: page alignment is sufficient for region and slot
  boundaries.

## Migration

The vmem refactor must temporarily preserve scheduler behavior before the arena
exists:

1. Replace each complete coroutine allocation with `reserve()` followed by
   `commit()`.
2. Replace stack reset with `decommit()` followed by `commit()` before reuse,
   where the current pool requires the slot to remain immediately reusable.
3. Replace complete deallocation with `release()`.
4. Preserve the existing guard-page protection call. Removing it belongs to
   the coroutine arena migration, where all slots share larger regions and
   per-slot protection would recreate Linux VMA growth.

## Verification

Add or update platform-vmem tests to cover:

- reserve, commit, write, decommit, recommit, and release;
- recommitted memory is writable without relying on preserved contents;
- multiple page-aligned subranges within one reservation;
- invalid or unsupported operations return `-1` where they can be tested
  portably;
- ASAN builds can reuse a decommitted and recommitted range without stale
  poison reports.

Run the existing runtime tests after migrating scheduler callers. Linux VMA
behavior should also be checked by repeatedly decommitting slots within one
region and confirming that the mapping count does not grow per slot.
