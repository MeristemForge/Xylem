# Container Design

Xylem ships two parallel families of containers: a set of **intrusive**
variants used internally on hot paths, and a set of **non-intrusive**
`xylem_`-prefixed wrappers that are the public API. This document covers both,
how they relate, and the rationale for keeping two flavors.

Sources: public headers in `include/xylem/container/`, implementations in
`src/container/xylem-*.c`; internal intrusive headers in `src/container/`.

Background on the public/internal split and the `*_entry()` convention:
[`../conventions.md`](../conventions.md) §7.

## 1. Two flavors, one idea

| | Intrusive (internal) | Non-intrusive (public) |
|---|----------------------|------------------------|
| Prefix | none (`list_t`, `heap_t`) | `xylem_` (`xylem_list_t`) |
| Node storage | embedded in caller's struct | allocated wrapper holds `void* data` |
| Allocation | zero per element | one node alloc per element |
| Recover element | `*_entry(node, type, member)` macro | API returns the stored `void*` |
| Audience | runtime, protocol code | application code |
| Header | `src/container/<c>.h` | `include/xylem/container/xylem-<c>.h` |

The two share the same algorithms; they differ only in *who owns the node
memory*. Intrusive containers put the linkage inside the element, so insertion
never allocates and an element can be removed in O(1) given its node. The public
wrappers trade that for a friendlier `void*`-based API.

```c
/* intrusive: node lives in the element, recovered by pointer arithmetic */
typedef struct { int v; list_node_t link; } item_t;
list_t lst;  list_init(&lst);
list_insert_tail(&lst, &it->link);
item_t* it = list_entry(node, item_t, link);

/* non-intrusive: stores a void*, allocates the node for you */
xylem_list_t* lst = xylem_list_create();
xylem_list_insert_tail(lst, ptr);
void* ptr = xylem_list_head(lst);
```

## 2. The catalog

| Container | Intrusive type | Public type | Notes |
|-----------|----------------|-------------|-------|
| Doubly-linked list | `list_t` / `list_node_t` | `xylem_list_t` | Sentinel head; O(1) head/tail/remove/swap. |
| Stack (LIFO) | `lifo_t` / `lifo_node_t` | `xylem_stack_t` | Singly-linked. |
| Queue (FIFO) | `queue_t` / `queue_node_t` | `xylem_queue_t` | Built on `list` (`queue_node_t == list_node_t`). |
| Binary heap | `heap_t` / `heap_node_t` | `xylem_heap_t` | Pointer-based min-heap by comparator. |
| Red-black tree | `rbtree_t` / `rbtree_node_t` | `xylem_rbtree_t` | Sorted, O(log n), unique keys. |
| Ring buffer | — | `xylem_ringbuf_t` | Public-only; power-of-two byte ring. |
| MPSC queue | `mpsc_t` / `mpsc_node_t` | — | Internal-only; lock-free, see §6. |

Two containers exist in only one flavor: the **ring buffer** is public-only (no
internal consumer needs it), and the **MPSC queue** is internal-only (it backs
the scheduler's deferred-post path).

## 3. Public (non-intrusive) API shape

All public containers follow the same lifecycle and error conventions
(see [`../conventions.md`](../conventions.md) §3, §5):

- `xylem_<c>_create(...)` → handle or `NULL`; `xylem_<c>_destroy(h)` frees node
  memory (not the user's `data`).
- Inspectors: `empty`, `len`, plus container-specific peeks
  (`head`/`tail`, `peek`, `front`, `root`, `first`/`last`).
- Mutators return `0`/`-1` when they allocate (`insert`, `push`, `enqueue`),
  `void` when they can't fail (`pop`, `dequeue`, `clear`).
- `data` is an opaque `void*` owned by the caller — containers never free it.
  `destroy`/`clear` drop node bookkeeping only.

Comparator-based containers take function pointers at create time:

- **Heap:** `xylem_heap_cmp_fn_t(a, b)` — negative if `a` outranks `b`.
- **Rbtree:** two comparators — `cmp_dd(a, b)` for ordering stored elements and
  `cmp_kd(key, data)` for lookups, so `find`/`erase` can take a bare key rather
  than a full element.

### Ring buffer specifics

`xylem_ringbuf_create(esize, bufsize)` allocates a fixed, byte-backed ring whose
capacity is rounded **down** to the largest power-of-two entry count that fits
in `bufsize` (so the index math is a mask, not a modulo). It is the only public
container with `write`/`read`/`peek` bulk semantics (returns the count actually
moved) and `full`/`avail`/`cap` inspectors. It copies bytes by value, unlike the
pointer-storing containers.

## 4. Internal (intrusive) API shape

Intrusive containers are `*_init(&c)` on caller-owned storage (no allocation),
operate on `*_node_t*`, and expose iteration the wrappers don't need to:

- `list` exposes `next`/`prev`/`sentinel` for traversal; `swap` enables the
  "swap with an empty list to drain atomically" idiom.
- `rbtree` exposes `next`/`prev`/`min`/`max` for in-order walks and uses
  node-node / key-node comparators (`cmp_nn` / `cmp_kn`).
- `heap` exposes `peek`/`remove`/`dequeue`; the scheduler's timer wheel is a
  `heap_t` keyed by absolute expiry.
- `queue` is a thin alias over `list` (`queue_node_t` *is* `list_node_t`,
  `queue_entry` *is* `list_entry`), so the runtime can move nodes between a
  list view and a queue view without copying.

The `*_entry(ptr, type, member)` macro is the recovery primitive across all of
them — `offsetof`-based, zero-cost, and **internal** (never exposed publicly).

## 5. Where the intrusive variants are used

These aren't academic; they carry the runtime:

- `runq` (global run queue) is a `queue_t` of coroutine contexts.
- Per-worker **timer heaps** are `heap_t` keyed on expiry.
- The scheduler **coroutine registry** is a `list_t`.
- Deferred posts ride the `mpsc_t` (next section).

Choosing intrusive here is deliberate: scheduling a coroutine must not allocate,
and a node already lives inside the coroutine context, so linking it in is a few
pointer writes.

## 6. MPSC queue — the one concurrent container

`mpsc_t` is a lock-free **multi-producer / single-consumer** intrusive queue
(Vyukov-style) used for `scheduler_post()`. Two contracts matter and are easy to
get wrong:

- **`mpsc_pop()` returning NULL does not mean "empty".** A producer may have
  claimed the tail slot but not yet linked its node, leaving the queue
  *temporarily inconsistent*. Consumers must retry from an outer loop (the
  scheduler does this on its poll cycle) rather than treat NULL as "drained".
- **Single consumer only.** `push` is multi-producer safe via an atomic tail
  exchange; `pop` must be called from one thread at a time. The scheduler
  enforces this with a `post_draining` CAS so only one worker drains at once.

Every other container in this document is **not** thread-safe; callers
serialize access (the runtime does so by confining most structures to a single
worker, or guarding them with a lock/spinlock as in the timer heaps).

## 7. Design rationale

- **Why two flavors?** Hot internal paths can't afford a malloc per insert and
  benefit from O(1) removal by node; application code prefers not to embed nodes
  and reason about `offsetof`. Keeping both lets each side pay only for what it
  needs.
- **Why share algorithms?** `queue`-on-`list` and `queue_entry`-as-`list_entry`
  mean one tested implementation backs multiple views.
- **Why `void*` (not macros/templates) for the public side?** It keeps the
  public ABI stable and the headers small, at the cost of one indirection — an
  acceptable trade for application-level use.

## 8. Related docs

- Conventions (public/internal, `*_entry`): [`../conventions.md`](../conventions.md)
- Runtime consumers (runq, timer heap, registry, posts): [`runtime.md`](runtime.md)
- Tests: [`../test/strategy.md`](../test/strategy.md) *(planned)*
