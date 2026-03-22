# Build Instructions

This guide covers building, testing, and generating coverage reports on Windows and Unix using CMake.
The exact commands depend on whether your CMake generator is single-config (e.g., Ninja, Unix Makefiles) or multi-config (e.g., Visual Studio, Ninja Multi-Config).

## Prerequisites

- CMake >= 3.16
- A C11-compatible compiler:
  - Windows: MSVC (Visual Studio 2022+) or Clang-cl
  - Linux/macOS: GCC >= 7 or Clang >= 6
- (Optional) For code coverage:
  - Linux: `lcov` and `genhtml` (`sudo apt install lcov`)
  - Windows: OpenCppCoverage (install via `scripts/install-deps.ps1`)

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
cmake -B out -D<PROJECT>_ENABLE_ASAN=ON
cmake --build out
```

| Sanitizer | What it catches | Option |
|-----------|----------------|--------|
| ASAN | Buffer overflow, use-after-free, memory leaks | `-D<PROJECT>_ENABLE_ASAN=ON` |
| TSAN | Data races, deadlocks | `-D<PROJECT>_ENABLE_TSAN=ON` |
| UBSAN | Undefined behavior (signed overflow, null deref, etc.) | `-D<PROJECT>_ENABLE_UBSAN=ON` |

ASAN and TSAN cannot be enabled simultaneously. Run them in separate builds.

## Code Coverage

Coverage requires `<PROJECT>_ENABLE_TESTING=ON` and `<PROJECT>_ENABLE_COVERAGE=ON`.

### Linux/macOS

```bash
cmake -B out -D<PROJECT>_ENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build out -j 8
cmake --build out --target coverage
```

Requires `lcov` and `genhtml`.

### Windows

```bash
cmake -B out -D<PROJECT>_ENABLE_COVERAGE=ON
cmake --build out --config Debug
cmake --build out --target coverage
```

Requires OpenCppCoverage.

HTML report is generated at `out/coverage/`.

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