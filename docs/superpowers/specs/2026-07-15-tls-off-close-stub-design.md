# TLS-off Close Stub Design

## Scope

Fix TLS-disabled builds that fail to link after mux began storing a transport
close callback. Also rename the scheduler's coroutine-slot deallocation helper
to match the platform vmem terminology.

## Design

`src/net/tls/xylem-tls-stub.c` will define `xylem_tls_close` alongside the
existing read and write stubs. The function will accept the opaque TLS
connection pointer, ignore it, and return. This preserves the current TLS-off
transport-table contract without exposing the CMake TLS feature flag inside
mux or pretending the rest of the TLS public API is available.

`_sched_coro_free` will be renamed to `_sched_coro_dealloc`, including all
internal call sites. Its behavior remains unchanged: external stacks use
`free`, while platform virtual-memory slots use `platform_vmem_dealloc`.

## Verification

The existing TLS-off full build is the regression test: before the fix, test
executables fail to link with an unresolved `xylem_tls_close`; after the fix,
all configured targets must link. The full Debug test suite must then pass.
Static checks will confirm that `_sched_coro_free` no longer exists and that
the modified C files meet the project style rules.
