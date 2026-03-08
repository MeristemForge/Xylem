---
inclusion: manual
---

# Debugging

## Diagnosis Order

1. Read the error message — compiler warnings, sanitizer output, assertion failures
2. Reproduce with a minimal test case (`tests/test-<module>.c`)
3. Run with sanitizers (ASAN → TSAN → UBSAN) to catch memory/thread/UB issues
4. Use a debugger only if sanitizers don't reveal the cause

## Sanitizer Constraints

Build commands are in `build.md`. Additional constraints:

- ASAN and TSAN cannot be enabled simultaneously
- Always build in Debug mode for full symbol info (`-C Debug` on Windows)
- ASAN is available on all platforms (MSVC, GCC, Clang). TSAN and UBSAN are GCC/Clang only.
- ASAN leak detection (`detect_leaks`) only works on Linux. On Windows use Debug CRT instead.

## Common Pitfalls

| Issue | Cause | Fix |
|-------|-------|-----|
| Crash only in Release | Uninitialized variable (zeroed in Debug, garbage in Release) | Run ASAN in Debug, inspect the variable |
| Works on Linux, crashes on Windows | `long` is 4 bytes on Windows x64, 8 on Linux | Use `int32_t`/`int64_t` instead of `long` |
| Segfault with no ASAN output | Stack overflow (default 1MB on Windows, 8MB on Linux) | Reduce recursion or increase stack size |
| Core dump not generated (Linux) | `ulimit -c` is 0 | `ulimit -c unlimited` before running |
| False positive in TSAN | Custom lock/synchronization not recognized | Use TSAN annotations (`__tsan_acquire`/`__tsan_release`) |

## Analysis Procedures

### Crash (segfault, access violation)

1. Reproduce in Debug build (`-C Debug`)
2. Run with ASAN — if it reports, follow the ASAN output (file, line, allocation trace)
3. If ASAN is clean, check for stack overflow (reduce recursion, print stack depth)
4. Attach debugger, get backtrace (`bt` in GDB/LLDB, `k` in CDB)

### Memory Leak

1. Run with ASAN (`detect_leaks=1` is default on Linux)
2. ASAN reports allocation site — trace the code path that skips `free`/`destroy`/`deinit`
3. Check early returns and error paths — common to miss cleanup on error branches

### Data Race / Deadlock

1. Run with TSAN — it reports the two conflicting accesses with stack traces
2. Identify the shared resource and which lock (if any) protects it
3. For deadlock: check lock ordering — all code paths must acquire locks in the same order

### Wrong Output (no crash)

1. Write a minimal test case that asserts the expected value
2. Add `printf` / logging at key decision points to trace the logic flow
3. Check integer overflow, signedness, and type promotion:
   - `unsigned - unsigned` wraps to large positive on underflow
   - Signed/unsigned comparison: signed value is implicitly converted to unsigned
   - `int32_t * int32_t` may overflow — cast one operand to `int64_t` first
4. Check platform-specific behavior (`long` size, endianness, path separators)
