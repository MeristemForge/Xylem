# Code Style

## Naming

| Category | Pattern | Example |
|----------|---------|---------|
| Public functions | `xylem_<module>_<action>` | `xylem_heap_insert` |
| Types | `xylem_<module>_t` | `xylem_list_t` |
| Node types | `xylem_<module>_node_t` | `xylem_heap_node_t` |
| Function pointer typedefs | `xylem_<module>_<purpose>_fn_t` | `xylem_rbtree_cmp_fn_t` |
| Internal/static helpers | `_<name>` prefix | `_heap_node_swap` |
| Source files | `xylem-<module>.c`, `xylem-<module>.h` | `xylem-list.c` |
| Test files | `test-<module>.c` | `test-list.c` |

## Formatting

clang-format with LLVM base style:
- 4-space indent, no tabs
- Pointer left-aligned (`int* p`)
- No bin-packing of arguments/parameters
- Aligned consecutive declarations
- No single-line functions

## Comments

### Public API (Header Files)

All `extern` function declarations must have a Doxygen `/** ... */` block:

```c
/**
 * @brief One-line summary.
 *
 * Optional details about behavior or algorithm.
 *
 * @param name   Description of the parameter.
 * @param name2  Aligned with other @param entries.
 *
 * @return Return value and error conditions.
 *
 * @note Caller responsibilities, buffer sizing, etc.
 */
```

Rules:
- Use `@brief`, `@param`, `@return`, `@note` tags
- Align `@param` descriptions
- Pure ASCII only — use `->` not `→`, `>=` not `≥`
- Blank comment lines between sections
- Concise, no filler

### Internal / Static Functions

No Doxygen required. A short `/* ... */` one-liner if the name isn't self-explanatory:

```c
/* Swap two heap nodes and update their positions. */
static inline void _heap_node_swap(...) { ... }
```

### Inline Comments

- `/* ... */` style (C11 compatible)
- Only where code is non-obvious
- Keep short
