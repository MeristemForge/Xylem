# Project Structure

```
include/
  xylem.h                      # Umbrella header — includes all module headers
  xylem/xylem-<module>.h       # Public API (one header per module)
  deprecated/                  # Legacy/compat headers
src/
  xylem-<module>.c             # Implementation (one source per module)
  platform/platform.h          # Platform-specific abstractions
tests/
  assert.h                     # Custom ASSERT macro
  test-<module>.c              # Unit tests (one file per module)
  CMakeLists.txt               # Test registration via xylem_add_test()
examples/
  CMakeLists.txt               # Example programs (currently empty)
cmake/
  xylem-utils.cmake            # Helpers: sanitizer setup, test registration
docs/
  build.md                     # Build instructions
  FAQ.md                       # Frequently asked questions
  images/                      # Project images and logos
```

## Adding a New Module

1. Create `include/xylem/xylem-<module>.h` — use `_Pragma("once")`, include `"xylem.h"`, declare public API with `extern`
2. Create `src/xylem-<module>.c` — license header, `#include "xylem.h"`, implement functions
3. Add `src/xylem-<module>.c` to `SRCS` in root `CMakeLists.txt`
4. Add `#include "xylem/xylem-<module>.h"` to `include/xylem.h`
5. Create `tests/test-<module>.c` — test functions + `main()` calling them
6. Add `xylem_add_test(<module>)` to `tests/CMakeLists.txt`

## Intrusive Data Structures

Heap, rbtree, list, stack, queue, and similar modules use intrusive nodes. Users embed a `xylem_<module>_node_t` in their struct and recover the container with:

```c
xylem_<module>_entry(node_ptr, container_type, member_name)
```
