---
inclusion: fileMatch
fileMatchPattern: "**/CMakeLists.txt,**/*.cmake"
---

# Build System

## Stack
- C11, CMake >= 3.16
- Output: `out/`
- Custom helpers: `cmake/xylem-utils.cmake`

## Options

| Option | Default | Purpose |
|--------|---------|---------|
| `XYLEM_ENABLE_TESTING` | ON | Unit tests |
| `XYLEM_ENABLE_ASAN` | OFF | AddressSanitizer |
| `XYLEM_ENABLE_TSAN` | OFF | ThreadSanitizer |
| `XYLEM_ENABLE_UBSAN` | OFF | UndefinedBehaviorSanitizer |
| `XYLEM_ENABLE_DYNAMIC_LIBRARY` | OFF | Shared lib instead of static |
| `XYLEM_ENABLE_COVERAGE` | OFF | Code coverage |

## Commands
```bash
cmake -B out && cmake --build out
ctest --test-dir out -C Debug --output-on-failure  # Windows
ctest --test-dir out --output-on-failure           # Linux/macOS
```

## Troubleshooting
- Missing coverage tool: run `./scripts/install-deps.sh` (Linux/macOS) or `scripts/install-deps.ps1` (Windows)
- Linux/macOS needs: lcov, genhtml
- Windows needs: OpenCppCoverage

## Test Framework

Custom `ASSERT(expr)` macro in `tests/assert.h` — prints `file:line` and aborts on failure.

Tests are plain C executables: `main()` calls `static void test_*()` functions. Registered via `xylem_add_test(<name>)` in CMake, which creates `test-<name>` from `test-<name>.c` linked against `xylem`.
