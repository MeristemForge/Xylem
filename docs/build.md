# Build Instructions

This guide covers building, testing, and generating coverage reports on Windows and Unix using CMake.
The exact commands depend on whether your CMake generator is single-config (e.g., Ninja, Unix Makefiles) or multi-config (e.g., Visual Studio, Ninja Multi-Config).

## Prerequisites

- CMake >= 3.25
- A C11-compatible compiler:
  - Windows: MSVC (Visual Studio 2022+) or Clang-cl
  - Linux/macOS: GCC >= 7 or Clang >= 6
- (Optional) For TLS/DTLS support: OpenSSL >= 3.5
- (Optional) For code coverage:
  - Linux: `lcov` and `genhtml` (`sudo apt install lcov`)
  - Windows: OpenCppCoverage (`winget install OpenCppCoverage.OpenCppCoverage`)

## Windows: Developer Command Prompt

On Windows, all commands (cmake, ctest, etc.) must be run from a Visual Studio Developer Command Prompt, or from a terminal where `vcvarsall.bat` has been executed. This ensures the MSVC compiler (`cl.exe`) and related tools are on `PATH`.

Common ways to open one:
- Start Menu → "Developer Command Prompt for VS 2022"
- Start Menu → "Developer PowerShell for VS 2022"
- Manually run: `"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64`

If using Clang-cl, the same environment is required — Clang-cl depends on MSVC headers and libraries.

## Configure the Build

### Generator Types

| Platform | Default Generator | Type |
|----------|-------------------|------|
| Windows | Visual Studio | Multi-config |
| Linux/macOS | Unix Makefiles | Single-config |
| Any (explicit) | Ninja | Single-config |
| Any (explicit) | Ninja Multi-Config | Multi-config |

You can force a specific generator with `-G "Generator Name"`.

### Multi-Config Generators

Supports Debug, Release, etc. in one build directory.

```bash
cmake -B out
cmake -B out -G "Visual Studio 17 2022"
cmake -B out -G "Ninja Multi-Config"
```

### Single-Config Generators

One build type per directory — specify at configure time.

```bash
cmake -B out -DCMAKE_BUILD_TYPE=Debug
cmake -B out -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

Common values for `CMAKE_BUILD_TYPE`: Debug, Release, RelWithDebInfo, MinSizeRel

### Feature Options

| Option | Default | Description |
|--------|---------|-------------|
| `XYLEM_ENABLE_TESTING` | OFF | Enable unit tests |
| `XYLEM_ENABLE_TLS` | OFF | Enable TLS/DTLS support (requires OpenSSL >= 3.5) |
| `XYLEM_ENABLE_DYNAMIC_LIBRARY` | OFF | Build shared library instead of static |
| `XYLEM_ENABLE_ASAN` | OFF | Address sanitizer |
| `XYLEM_ENABLE_TSAN` | OFF | Thread sanitizer |
| `XYLEM_ENABLE_UBSAN` | OFF | Undefined behavior sanitizer |
| `XYLEM_ENABLE_COVERAGE` | OFF | Code coverage reporting |

Example — enable testing and TLS:

```bash
cmake -B out -DXYLEM_ENABLE_TESTING=ON -DXYLEM_ENABLE_TLS=ON
```

## Build

### Multi-Config
```bash
cmake --build out --config Debug -j 8
```

### Single-Config
```bash
cmake --build out -j 8
```

## Run Tests

Tests require `XYLEM_ENABLE_TESTING=ON` at configure time.

### Multi-Config
```bash
ctest --test-dir out -C Debug --output-on-failure
```

### Single-Config
```bash
ctest --test-dir out --output-on-failure
```

### Running a Single Test

```bash
ctest --test-dir out -R <module> --output-on-failure
```

Example: `ctest --test-dir out -R list --output-on-failure` runs only `test-list`.

## Sanitizers

Run the full test suite with each sanitizer to catch different classes of bugs.

```bash
cmake -B out -DXYLEM_ENABLE_TESTING=ON -DXYLEM_ENABLE_ASAN=ON
cmake --build out
ctest --test-dir out --output-on-failure
```

| Sanitizer | What it catches | Option | Windows |
|-----------|----------------|--------|---------|
| ASAN | Buffer overflow, use-after-free, memory leaks | `-DXYLEM_ENABLE_ASAN=ON` | Yes |
| TSAN | Data races, deadlocks | `-DXYLEM_ENABLE_TSAN=ON` | No |
| UBSAN | Undefined behavior (signed overflow, null deref, etc.) | `-DXYLEM_ENABLE_UBSAN=ON` | No |

ASAN and TSAN cannot be enabled simultaneously. Run them in separate builds.

### Windows ASAN Note

When running ASAN-instrumented binaries on Windows, always use a Developer Command Prompt (e.g., x64 Native Tools Command Prompt for VS 2022). This environment sets up the runtime paths for `clang_rt.asan_dynamic-x86_64.dll`. Running from a plain terminal will fail with a missing DLL error.

### TSAN on Linux

TSAN requires GCC 13+ / glibc 2.36+ to correctly intercept C11 `thrd_create`. On older toolchains, glibc's `thrd_create` calls `pthread_create` via an internal direct call that bypasses TSAN's interception. This project works around the issue by using a pthread-based polyfill instead of glibc's `<threads.h>`.

When running under WSL, TSAN may report `FATAL: ThreadSanitizer: unexpected memory mapping` due to WSL's non-standard ASLR behavior. Use `setarch -R` to disable ASLR:

```bash
setarch -R ctest --test-dir out --output-on-failure
```

This is not needed on native Linux or macOS.

## Code Coverage

Coverage requires `XYLEM_ENABLE_TESTING=ON` and `XYLEM_ENABLE_COVERAGE=ON`.

### Linux/macOS

```bash
cmake -B out -DXYLEM_ENABLE_TESTING=ON -DXYLEM_ENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build out -j 8
cmake --build out --target coverage
```

Requires `lcov` and `genhtml`. HTML report is generated at `out/coverage/html/`.

### Windows

```bash
cmake -B out -DXYLEM_ENABLE_TESTING=ON -DXYLEM_ENABLE_COVERAGE=ON
cmake --build out --config Debug
cmake --build out --target coverage
```

Requires OpenCppCoverage. HTML report is generated at `out/coverage/`.

## Install

### Multi-Config
```bash
cmake --install out --config Debug
```

### Single-Config
```bash
cmake --install out
```

## Quick Reference

| Step | Multi-Config | Single-Config |
|------|-------------|---------------|
| Configure | No `-DCMAKE_BUILD_TYPE` | Must set `-DCMAKE_BUILD_TYPE=Debug` |
| Build | `--build ... --config Debug` | `--build ...` (no `--config`) |
| Test | `ctest ... -C Debug` | `ctest ...` (no `-C`) |
| Install | `--install ... --config Debug` | `--install ...` (no `--config`) |

> For CI scripts: always pass `--config` and `-C` — they are safely ignored on single-config generators.
