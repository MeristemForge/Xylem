---
inclusion: fileMatch
fileMatchPattern: "**/CMakeLists.txt,**/*.cmake"
---

# Tech Stack & Build

## Language & Standard

- C11 (`CMAKE_C_STANDARD 11`, extensions enabled)
- `_Pragma("once")` instead of traditional include guards
- C11 atomics (Windows requires `/experimental:c11atomics`)

## Build System

- CMake >= 3.16
- Output directory: `out/`
- Custom helpers: `cmake/xylem-utils.cmake`

## CMake Options

| Option | Default | Purpose |
|--------|---------|---------|
| `XYLEM_ENABLE_TESTING` | ON | Unit tests |
| `XYLEM_ENABLE_ASAN` | OFF | AddressSanitizer |
| `XYLEM_ENABLE_TSAN` | OFF | ThreadSanitizer |
| `XYLEM_ENABLE_UBSAN` | OFF | UndefinedBehaviorSanitizer |
| `XYLEM_ENABLE_DYNAMIC_LIBRARY` | OFF | Shared lib instead of static |
| `XYLEM_ENABLE_COVERAGE` | OFF | Code coverage |

## Common Commands

```bash
# Configure & build (tests enabled by default)
cmake -B out
cmake --build out

# Disable tests
cmake -B out -DXYLEM_ENABLE_TESTING=OFF

# Run tests — multi-config (Windows/MSVC)
ctest --test-dir out -C Debug --output-on-failure

# Run tests — single-config (Linux/macOS)
ctest --test-dir out --output-on-failure

# Sanitizer build
cmake -B out -DXYLEM_ENABLE_ASAN=ON
cmake --build out

# Coverage (Linux)
cmake -B out -DXYLEM_ENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build out
cmake --build out --target coverage
# Report: out/coverage/
```

## Test Framework

Custom `ASSERT(expr)` macro in `tests/assert.h` — prints `file:line` and aborts on failure.

Tests are plain C executables: `main()` calls `static void test_*()` functions. Registered via `xylem_add_test(<name>)` in CMake, which creates `test-<name>` from `test-<name>.c` linked against `xylem`.
