# Minicoro Windows ASM Boundary Design

## Context

Xylem extends minicoro so Windows x64 ASM coroutines can use page-granular
lazy stack commitment inside arena slots. The initial implementation also
exposed the physical stack offset for every embedded-stack backend and changed
their context creation code to use a shared offset helper.

Only Windows x64 ASM needs this information. Unix commits the complete arena
slot, Windows Fiber owns an external stack, and the WebAssembly backends do not
use the Windows virtual-memory layout. Keeping cross-backend stack introspection
therefore expands the bundled third-party diff without serving the runtime.

## Decision

Keep these declarations available for every backend:

```c
MCO_API size_t mco_desc_stack_offset(const mco_desc* desc);
MCO_API void* mco_get_stack_limit(const mco_coro* co);
MCO_API void mco_set_stack_limit(mco_coro* co, void* stack_limit);
```

Their contract is platform-management capability rather than universal layout
introspection:

- Windows x64 ASM returns the arena-embedded stack offset and saved TEB
  `StackLimit`.
- Every other backend returns `0` from `mco_desc_stack_offset()`, returns `NULL`
  from `mco_get_stack_limit()`, and treats `mco_set_stack_limit()` as a no-op.

This keeps `coro.c` platform-neutral while limiting minicoro internals to the
backend that needs them.

## Windows x64 ASM Layout

The descriptor keeps metadata and stack regions page-separated:

```text
slot base
  metadata and storage
  page-alignment padding
  reserved stack region
slot end
```

`desc->stack_size` is page aligned. `desc->coro_size` is the page-aligned stack
offset plus the stack size. Context creation locates the stack through that
offset instead of placing it immediately after storage.

The initial context records:

- `StackBase` as the true top of the complete stack region;
- `StackLimit` as the initial committed frontier;
- `DeallocationStack` as the fixed low address of the stack region.

The 32-byte x64 shadow space affects the initial `RSP`, but does not reduce
`StackBase`.

## Page Size Provider

Minicoro does not query the operating system for page size. The translation
unit that enables `MINICORO_IMPL` must provide this function-like macro for the
Windows Fiber and Windows x64 ASM backends:

```c
#define MCO_GET_PAGE_SIZE() platform_vmem_page_size()
```

`runtime.c` defines the macro immediately before including the implementation.
Both backends call the macro when constructing their descriptor or Fiber. A
missing provider on either Windows backend is a compile-time error.

The platform vmem module owns system discovery and caching. This removes the
Windows ASM implementation's page-size dependency on `windows.h`; Windows
Fiber continues to include that header for `CreateFiberEx()` and related APIs.

## Other Backends

Restore the upstream address and size calculations for Unix ASM/ucontext,
Windows Fiber, Emscripten Fiber, and Asyncify. They do not define an internal
stack-offset helper for Xylem and do not change context creation for this
feature.

Windows Fiber continues to allocate its stack through `CreateFiberEx()`.
Unix continues to commit the complete arena slot. The WebAssembly backends
continue to use their original embedded-stack calculations internally.

## Runtime Integration

`coro.c` continues to call the three functions without platform conditionals.
An offset of zero means there is no stack region for the Xylem platform layer
to manage separately. The Unix platform implementation commits the complete
slot, while Windows Fiber follows the same external-stack path.

Only Windows x64 ASM passes a nonzero `stack_low` and `stack_size` to
`platform_coro_prepare_initial_layout()`, enabling metadata commitment, initial
guard setup, lazy stack growth, and hot `StackLimit` preservation.

## Verification

Tests must establish:

- Windows x64 ASM reports a nonzero, page-aligned stack offset.
- Windows x64 ASM initializes `StackBase`, `StackLimit`, and guard pages using
  the page-separated layout.
- Hot reuse preserves a grown `StackLimit`; cold arena reuse restores the
  initial layout.
- Every other backend reports offset zero and a null stack limit.
- Existing coroutine creation, execution, reuse, and destruction behavior is
  unchanged on all backends.

The generated comparison patch must be regenerated against commit `d456770f`
after implementation so it contains only the required minicoro changes.

## Non-Goals

- Providing universal physical stack introspection for all minicoro backends.
- Moving Windows Fiber stacks into arena slots.
- Changing Unix or WebAssembly virtual-memory behavior.
- Changing the public Xylem API.
