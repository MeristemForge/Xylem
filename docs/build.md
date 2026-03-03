# Build Instructions

This guide covers building, testing, and generating coverage reports for the project on **Windows**, **Unix** using CMake.  
The exact commands depend on whether your CMake generator is **single-config** (e.g., `Ninja`, `Unix Makefiles`) or **multi-config** (e.g., `Visual Studio`, `Ninja Multi-Config`).

---

## 🛠️ Prerequisites

- **CMake ≥ 3.19**
- A C11-compatible compiler:
  - **Windows**: MSVC (Visual Studio 2019+) or Clang-cl
  - **Linux/macOS**: GCC ≥ 7 or Clang ≥ 6
- (Optional) For code coverage (**Linux only**):
  - Install `lcov` and `genhtml`  
    ```bash
    sudo apt install lcov      # Debian/Ubuntu
    ```

---

## 🔧 Configure the Build

### Determine Your Generator Type

| Platform      | Default Generator       | Type          |
|---------------|-------------------------|---------------|
| Windows       | Visual Studio           | Multi-config  |
| Linux/macOS   | Unix Makefiles          | Single-config |
| Any (explicit)| `Ninja`                 | Single-config |
| Any (explicit)| `Ninja Multi-Config`    | Multi-config  |

> 💡 You can force a specific generator with `-G "Generator Name"`.

### ✅ Configuration Commands

#### ➤ Multi-Config Generators  
(Supports `Debug`, `Release`, etc. in one build directory)

```bash
# Windows (default)
cmake -B out

# Or explicitly (e.g., VS 2022)
cmake -B out -G "Visual Studio 17 2022"

# Cross-platform multi-config (CMake ≥ 3.19)
cmake - B out -G "Ninja Multi-Config"
```

#### ➤ Single-Config Generators  
(One build type per directory — specify it at configure time)

```bash
# Linux/macOS (default: Makefiles)
cmake -B out -DCMAKE_BUILD_TYPE=Debug

# Faster alternative: Ninja
cmake -B out -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

> 📌 Common values for `CMAKE_BUILD_TYPE`:  
> `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel`

---

## 🏗️ Build

### Multi-Config
```bash
cmake --build out --config Debug -j 8
```

### Single-Config
```bash
cmake --build out -j 8
# Build type was fixed during configure (e.g., Debug)
```

---

## 🧪 Run Tests

### Multi-Config
```bash
ctest --test-dir out -C Debug --output-on-failure
```

### Single-Config
```bash
ctest --test-dir out --output-on-failure
# No -C needed — only one configuration exists
```

---

## 📊 Generate Code Coverage Report (Linux Only)

First, enable coverage during configuration:

```bash
cmake -B out -DXYLEM_ENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build out -j 8
cmake --build out --target coverage
```

✅ The HTML report is generated at:  
```
out/coverage
```

> 💡 Requires `lcov` and `genhtml`

---

## 📦 Install

### Multi-Config
```bash
cmake --install out --config Debug
```

### Single-Config
```bash
cmake --install out
# Installs the only built configuration
```

---

## 🔄 Quick Reference: Command Differences

| Step       | Multi-Config                          | Single-Config                        |
|------------|---------------------------------------|--------------------------------------|
| Configure  | No `-DCMAKE_BUILD_TYPE`               | Must set `-DCMAKE_BUILD_TYPE=Debug` |
| Build      | `--build ... --config Debug`          | `--build ...` (no `--config`)       |
| Test       | `ctest ... -C Debug`                  | `ctest ...` (no `-C`)               |
| Install    | `--install ... --config Debug`        | `--install ...` (no `--config`)     |

---

> ✅ **Tip for CI Scripts**:  
> To write cross-platform scripts, always pass `--config` and `-C` — they are safely ignored on single-config generators.

--- 

*Happy building!* 🚀

--- 

