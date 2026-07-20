# Windows ASan Lazy Stack Design

## Goal

Keep Windows x64 ASM coroutine stacks lazily committed while preventing ASan user-poison from rejecting pages that Windows commits through a moving `PAGE_GUARD`.

## Design

Arena-owned cold slots remain decommitted and poisoned. When a slot is prepared for a coroutine, the platform commits the metadata and initial stack pages as before, then unpoisons the complete slot in ASan shadow memory. The unpoisoning is logical ownership of the slot; it does not commit physical pages or change page protections. Windows can therefore move `PAGE_GUARD` into previously reserved pages without an ASan false positive.

When a coroutine slot returns to the arena, the existing decommit-and-poison path remains unchanged. Hot-pool reuse does not reset or decommit the slot, so it has no additional VM cost.

## Verification

The Windows ASan coroutine test must execute a real stack-consuming path that crosses the initial guard page and then performs an instrumented allocation. The existing network tests remain the integration regression set. The focused test must fail before the change with `use-after-poison` and pass after it.

