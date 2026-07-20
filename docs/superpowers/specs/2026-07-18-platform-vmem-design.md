# Platform Virtual Memory Lifecycle Design

## Scope

Define one internal virtual-memory lifecycle for Linux, Android, macOS, iOS,
and Windows. The interface separates ownership of a complete address range
from commit and decommit transitions within that range.

## API

Keep the existing page-size query, expose four lifecycle transitions, and
provide the narrow guard operation required by Windows coroutine stacks:

```c
extern size_t platform_vmem_page_size(void);

extern void* platform_vmem_reserve(size_t size);

extern int platform_vmem_commit(
    void* ptr,
    size_t size);

extern int platform_vmem_guard(
    void* ptr,
    size_t size);

extern int platform_vmem_decommit(
    void* ptr,
    size_t size);

extern int platform_vmem_release(
    void* ptr,
    size_t size);
```

## Common Semantics

`reserve()` acquires one contiguous virtual address range. The returned base
and the size passed to all other functions must be system-page aligned.

`commit()` makes a reserved range available for read/write access. It is
idempotent and clears stale ASAN poison before returning the range to a caller.
The transition may update platform accounting without changing page
permissions.

`guard()` marks an already committed range as a native moving stack guard on
Windows. Windows guard protection is one-shot. Unix implementations return
success without changing page permissions because Unix coroutine stacks do not
use this mechanism.

`decommit()` makes the range reusable while preserving the containing
reservation. Previous contents become unspecified. The caller must not access
the range again until `commit()` succeeds, even on systems where the address
technically remains accessible. ASAN builds poison the range only after the OS
transition succeeds.

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
- `guard`: successful no-op.
- `decommit`: `madvise(MADV_FREE)`, then poison under ASAN.
- `release`: `munmap` the complete region.

The region is mapped read/write once. `MADV_FREE` permits lazy reclamation: old
bytes may remain until memory pressure causes the kernel to discard the pages.
Callers therefore treat all recommitted contents as unspecified.

After reserving the complete region, call `madvise(MADV_NOHUGEPAGE)` on the
whole mapping. Coroutine slots are independently discarded and are a poor fit
for transparent huge pages. This call is a hint; a rejected hint does not fail
the reservation. Applying it to the complete mapping avoids slot-granularity
VMA splits.

Do not use `MAP_NORESERVE`. Reservation failure remains observable and can be
handled by the arena's region-size fallback instead of surfacing as a later
fatal page fault.

### macOS and iOS

- `reserve`: one read/write anonymous private `mmap` for the complete region.
- `commit`: `madvise(MADV_FREE_REUSE)` and clear ASAN poison.
- `guard`: successful no-op.
- `decommit`: `madvise(MADV_FREE_REUSABLE)`, then poison under ASAN.
- `release`: `munmap` the complete region.

The Darwin reusable advice pair keeps kernel task accounting accurate while
allowing discarded pages to be reclaimed. No per-slot protection transition is
performed.

### Windows

- `reserve`: `VirtualAlloc(..., MEM_RESERVE, PAGE_NOACCESS)`.
- `commit`: `VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE)` and clear
  ASAN poison.
- `guard`: `VirtualProtect(..., PAGE_READWRITE | PAGE_GUARD)`.
- `decommit`: `VirtualFree(ptr, size, MEM_DECOMMIT)`, then poison under ASAN.
- `release`: `VirtualFree(ptr, 0, MEM_RELEASE)`.

Unused slots consume address space but no system commit charge. Only committed
slots are readable and writable.

## Verification

Add or update platform-vmem tests to cover:

- reserve, commit, write, decommit, recommit, and release;
- Windows guard protection and the Unix no-op contract;
- recommitted memory is writable without relying on preserved contents;
- multiple page-aligned subranges within one reservation;
- invalid or unsupported operations return `-1` where they can be tested
  portably;
- ASAN builds can reuse a decommitted and recommitted range without stale
  poison reports.

Run the existing runtime tests after migrating scheduler callers. Linux VMA
behavior should also be checked by repeatedly decommitting slots within one
region and confirming that the mapping count does not grow per slot.
