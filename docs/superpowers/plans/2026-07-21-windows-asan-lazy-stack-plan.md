# Windows ASan Lazy Stack Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Synchronize ASan shadow state with Windows lazily committed coroutine stack pages.

**Architecture:** Arena owns ASan shadow transitions for coroutine slots. It unpoisons a complete slot before allocation, then poisons it after successful decommit. Platform vmem operations expose ASan macros but never change shadow state implicitly.

**Tech Stack:** C11, CMake, Windows x64 ASM backend, MSVC AddressSanitizer, CTest.

---

### Task 1: Add a focused stack-growth regression

**Files:**
- Modify: `tests/test-coro.c`

- [ ] **Step 1: Add a test entry that consumes the real ASM coroutine stack and performs `calloc` at depth.** Keep the test Windows x64 ASM-only and use the existing coroutine creation helpers. The assertion is that the coroutine returns successfully and frees the allocation.
- [ ] **Step 2: Build and run the focused test with ASan.** Expected before the production change: `AddressSanitizer: use-after-poison`.

### Task 2: Make vmem shadow transitions explicit

**Files:**
- Modify: `src/platform/platform-vmem.h`
- Modify: `src/platform/unix/platform-vmem.c`
- Modify: `src/platform/win/platform-vmem.c`
- Modify: `src/runtime/arena.c`
- Modify: `tests/test-vmem.c`

- [ ] **Step 1: Remove implicit ASan operations from platform vmem implementations.** Keep `VMEM_ASAN_POISON` and `VMEM_ASAN_UNPOISON` available to callers.
- [ ] **Step 2: Pair arena allocation and reclamation with explicit ASan macros.** Unpoison before returning a slot, poison after successful decommit, and unpoison before region release.
- [ ] **Step 3: Run the focused test and the failing integration tests.** Expected: no ASan report.
- [ ] **Step 4: Run the complete Windows ASan CTest suite.** Expected: zero failed tests and zero sanitizer reports.

### Task 3: Review and commit

- [ ] **Step 1: Review the staged diff for scope, ASCII-only source, and no debug artifacts.**
- [ ] **Step 2: Commit with `refactor(platform): make asan shadow explicit`.**
