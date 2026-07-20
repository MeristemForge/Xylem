# Windows ASan Lazy Stack Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Synchronize ASan shadow state with Windows lazily committed coroutine stack pages.

**Architecture:** `platform_coro_prepare_slot()` owns the transition from a cold arena slot to an active coroutine slot. It keeps the existing metadata/guard/RW commit layout and unpoisons the full logical slot after successful setup. Arena release continues to decommit and poison the whole slot.

**Tech Stack:** C11, CMake, Windows x64 ASM backend, MSVC AddressSanitizer, CTest.

---

### Task 1: Add a focused stack-growth regression

**Files:**
- Modify: `tests/test-coro.c`

- [ ] **Step 1: Add a test entry that consumes the real ASM coroutine stack and performs `calloc` at depth.** Keep the test Windows x64 ASM-only and use the existing coroutine creation helpers. The assertion is that the coroutine returns successfully and frees the allocation.
- [ ] **Step 2: Build and run the focused test with ASan.** Expected before the production change: `AddressSanitizer: use-after-poison`.

### Task 2: Unpoison an active slot's complete logical range

**Files:**
- Modify: `src/platform/win/platform-coro.c:116-141`

- [ ] **Step 1: After metadata and initial stack setup succeed, call `VMEM_ASAN_UNPOISON(layout.slot_low, layout.slot_size)`.** Do not change reserve, commit, guard, or decommit behavior.
- [ ] **Step 2: Run the focused test and the failing integration tests.** Expected: no ASan report.
- [ ] **Step 3: Run the complete Windows ASan CTest suite.** Expected: zero failed tests and zero sanitizer reports.

### Task 3: Review and commit

- [ ] **Step 1: Review the staged diff for scope, ASCII-only source, and no debug artifacts.**
- [ ] **Step 2: Commit with `fix(runtime): sync asan with lazy coroutine stacks`.**

