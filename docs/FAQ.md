#### Q: When using AddressSanitizer (ASan) in Visual Studio 2022, the application runs fine within the IDE, but when executing from the command line, it reports an error about not being able to find `clang_rt.asan_dynamic-x86_64.dll`.

> **A:** ✅ Use the **Developer Command Prompt**, such as the **x64 Native Tools Command Prompt for VS 2022**.  
> This environment automatically sets up the necessary runtime paths for sanitizer DLLs.

---

#### Q: Why does ThreadSanitizer (TSan) report "FATAL: ThreadSanitizer: unexpected memory mapping" when running my program?

> **A:** ⚠️ This error occurs because TSan requires a fixed virtual memory layout. ASLR (Address Space Layout Randomization) interferes with this.  
> 💡 **Fix**: Disable ASLR using `setarch -R`:
> ```bash
> setarch -R ctest --test-dir out -C Debug --output-on-failure
> ```
> 📌 Note: TSan is **only supported on Unix-like systems** (Linux/macOS). It is **not available on Windows**.

---

#### Q: Can AddressSanitizer (ASan), ThreadSanitizer (TSan), and UndefinedBehaviorSanitizer (UBSan) be enabled simultaneously?

> **A:** ❌ **No.** These sanitizers are **mutually exclusive**. Enabling more than one can cause:
> - Build failures
> - Runtime crashes
> - Conflicting or misleading diagnostics  
> ✅ Always enable **only one sanitizer at a time**.

---

#### Q: How do I enable a specific sanitizer (ASan/TSan/UBSan) in my CMake project?

> **A:** Configure your build with the corresponding CMake option. For example:
> ```bash
> cmake -B out -DXYLEM_ENABLE_ASAN=ON    # ASan
> cmake -B out -DXYLEM_ENABLE_TSAN=ON    # TSan (Unix only)
> cmake -B out -DXYLEM_ENABLE_UBSAN=ON   # UBSan
> ```
> 🔧 Replace `XYLEM_ENABLE_*` with the actual option names used in your project.  
> ⚠️ Remember: **Only one sanitizer can be active at a time**.

---

#### Q: How do I generate a code coverage report for my tests?

> **A:** ✅ Coverage is supported on **Linux only**. Generating a report requires three steps:
>
> ```bash
> # 1. Configure with coverage enabled
> cmake -B out -DXYLEM_ENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
>
> # 2. Build
> cmake --build out -j 8
>
> # 3. Generate the HTML coverage report
> cmake --build out --target coverage
> ```
>
> 📁 The final report is available at `out/coverage`.  
> 💡 Requires `lcov` and `genhtml` (e.g., `sudo apt install lcov`).

--- 

### Symbol Usage Guide (for reference):

| Symbol | Meaning                     | When to Use                                      |
|--------|-----------------------------|--------------------------------------------------|
| ✅     | Correct / Recommended       | Solutions, best practices                        |
| ⚠️     | Warning / Caution           | Limitations, side effects, platform differences  |
| ❌     | Not allowed / Not supported | Incorrect usage, unsupported features            |
| 💡     | Tip / Helpful advice        | Practical suggestions, workarounds               |
| 📌     | Important note              | Key details, essential clarifications            |
| 🔧     | Configuration / Customization | User-adjustable options or settings            |
| 📁     | File path / Output location | Report directories, log files, build artifacts   |

