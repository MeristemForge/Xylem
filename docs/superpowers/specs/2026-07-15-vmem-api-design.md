# Virtual Memory API Simplification

## Goal

Reduce the internal virtual-memory API to the operations required by the
coroutine allocator while keeping their cross-platform semantics explicit.

## API

The platform layer will expose only these functions:

```c
extern size_t platform_vmem_page_size(void);
extern void* platform_vmem_alloc(size_t size);
extern void platform_vmem_reset(void* ptr, size_t size);
extern void platform_vmem_dealloc(void* ptr, size_t size);
extern int platform_vmem_protect(
    void* ptr,
    size_t size,
    platform_vmem_prot_t prot);
```

`platform_vmem_reserve` and `platform_vmem_commit` will be removed together.
They have no callers after the scheduler switched to `platform_vmem_alloc`.

## Semantics

- `page_size` returns the system page size.
- `alloc` creates a page-backed read/write region. It returns `NULL` on
  failure and resets stale ASAN shadow state on success.
- `reset` marks existing contents as no longer needed while preserving the
  mapping and its access permissions. Callers must treat previous contents as
  unspecified. The operation does not promise immediate zeroing or release of
  commit charge.
- `dealloc` destroys the entire region. The pointer becomes invalid.
- `protect` changes access permissions on an existing region and returns `0`
  on success or `-1` on failure.

## Platform Mapping

| API | Windows | Unix |
|-----|---------|------|
| `alloc` | `VirtualAlloc` with `MEM_RESERVE | MEM_COMMIT` | read/write anonymous `mmap` |
| `reset` | `VirtualAlloc` with `MEM_RESET` | `madvise` with `MADV_DONTNEED` |
| `dealloc` | `VirtualFree` with `MEM_RELEASE` | `munmap` |
| `protect` | `VirtualProtect` | `mprotect` |

## Scheduler Lifecycle

Fresh coroutine slots use `alloc`. A guard page is installed with `protect`.
If guard-page protection fails, the scheduler deallocates the new region and
reports allocation failure.

When a coroutine returns to the pool, only its reusable stack pages are reset.
The mapping, metadata, and guard-page protection remain in place. Reusing a
pooled slot does not require another allocation or commit operation.

When a pool rejects a slot or the scheduler is destroyed, the entire slot is
deallocated.

## Verification

Tests should cover allocation and read/write access, reset followed by reuse,
protection changes, and full deallocation on the native platform. Existing
scheduler tests must continue to pass on Windows and Unix implementations.
