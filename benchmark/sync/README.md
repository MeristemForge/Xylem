# Xylem Sync-Primitive Benchmark

Microbenchmarks for the xylem coroutine sync primitives, compared against
the equivalent constructs in Go (goroutines) and Rust (Tokio tasks).

| Primitive | Xylem | Go | Rust (Tokio) |
|-----------|:-----:|:--:|:------------:|
| mutex     | `xylem_mutex` | `sync.Mutex` | `tokio::sync::Mutex` |
| cond      | `xylem_cond`  | `sync.Cond`  | `tokio::sync::Notify` |
| waitgroup | `xylem_waitgroup` | `sync.WaitGroup` | `tokio::task::JoinSet` |
| sem       | `xylem_sem`   | buffered `chan` (token bucket) | `tokio::sync::Semaphore` |
| channel   | `xylem_channel` (MPSC) | buffered `chan` | `tokio::sync::mpsc` (unbounded) |

A sixth, xylem-only probe, `handoff`, measures the raw cross-context wake
latency (see "Cross-context direction" below) and has no Go/Rust column --
goroutines and Tokio tasks cannot block on the same object as an OS thread.

Unlike the protocol suites (`tcp/`, `udp/`, `tls/`), there is no client/server
or network I/O here: each binary runs one primitive entirely in-process and
prints a JSON result. The C, Go and Rust programs run the **identical**
workload so the numbers line up field-for-field.

## Concurrency modes (`--modes`)

Each primitive runs under up to three concurrency models. xylem's sync
primitives are *context-adaptive* (a blocking op parks a coroutine or blocks an
OS thread, and the two interoperate on the same object), so the **same** worker
code runs in every mode — only how workers are launched changes.

| Mode     | Workers are…            | xylem | go | rust |
|----------|-------------------------|:-----:|:--:|:----:|
| `coro`   | coroutines / async tasks | ✓ (`xylem_spawn`) | ✓ (goroutines) | ✓ (Tokio) |
| `thread` | plain OS threads         | ✓ | – | ✓ (`std::thread` + `std::sync`) |
| `mixed`  | half coroutines, half OS threads on one primitive | ✓ | – | – |

- **Go** has only goroutines, so a pure-thread or mixed model isn't expressible
  (user code always runs on a goroutine) — `go` runs `coro` only.
- **Rust** offers `coro` (Tokio + `tokio::sync`) and `thread` (`std::thread` +
  `std::sync`), but the two can't share one primitive (an async `Mutex`/`Notify`
  isn't usable from a blocking thread, and vice-versa) — no `mixed`.
- **xylem** is the only one that covers `mixed`: e.g. a coroutine producer
  handing off to an OS-thread consumer through the same `xylem_cond`.

The runner skips unsupported `(lang, mode)` cells automatically (the binaries
also reject them with a non-zero exit). Thread/mixed modes use lighter
per-primitive iteration counts since spawning an OS thread costs far more than
a coroutine (notably `waitgroup`, which spawns workers every round).

## Cross-context direction

When a coroutine and an OS thread block on the same object, the *direction* of
the wake matters a lot, and the two directions are far from symmetric:

| waker → waiter      | relative cost | why |
|---------------------|:-------------:|-----|
| coro → coro         | cheapest      | pure userspace reschedule |
| coro → thread       | cheap         | one `futex`/`WaitOnAddress` wake |
| thread → thread     | medium        | one `futex` wake |
| **thread → coro**   | **expensive** | a foreign thread must inject into the scheduler's global run queue, wake a parked worker, which then resumes (and often migrates) the coroutine |

Most primitives cannot expose a single direction in `mixed` mode:

- **mutex**, **sem** -- every worker both acquires and releases, so wakes go
  both ways (symmetric); direction is not separable.
- **cond**, **waitgroup** -- a round-trip (ping-pong / release-then-join), so
  every cycle pays *both* directions; pinning a role to a context only
  relabels the same total.
- **channel** -- the one naturally one-way case (senders only wake, the
  receiver only blocks), so `--chan-dir t2c|c2t` pins a clean direction.
  Because xylem's channel is unbounded, the receiver rarely sleeps, so both
  directions measure close: buffering amortizes the wake away.

To see the bare cost of each direction, use the `handoff` probe: two parties
ping-pong through a pair of binary semaphores (no mutex, no predicate), with
each party's vehicle pinned by `--ho-dir`:

```
sync-xylem handoff --ho-dir cc --iters 1000000   # coro  <-> coro
sync-xylem handoff --ho-dir ct --iters 1000000   # coro  <-> OS thread
sync-xylem handoff --ho-dir tt --iters 1000000   # thread <-> OS thread
```

`ns/op` is the round-trip wake latency. A representative run (Windows, ratios
matter more than absolutes): `cc` ~440 ns, `tt` ~1.3 us, `ct` ~12.8 us. Since
`ct = (coro→thread) + (thread→coro)` and `coro→thread` is about a futex
(~0.6 us), the `thread→coro` wake alone is ~12 us -- roughly 20x a
same-context wake. **Takeaway:** keep a high-frequency `thread→coro` wake off
the hot path; let the coroutine side block on a buffered channel and drain in
batches, or batch the thread-side signal, so one wake amortizes many items.

## Layout

```
benchmark/sync/
  xylem-sync/main.c      five primitives + the handoff probe (-> sync-xylem)
  go-sync/main.go        Go equivalent                       (-> sync-go)
  rust-sync/src/main.rs  Rust/Tokio equivalent               (-> sync-rust)

benchmark/out/           build output, shared with the net suite (gitignored):
                         binaries, build/ (CMake tree), results/<ts>/ JSON

benchmark/scripts/
  run-sync.sh            Linux/macOS driver (build/bench/all)
  run-sync.bat           Windows driver (run from a VS Dev Prompt)
```

## Quick Start

### Linux / macOS

```bash
cd benchmark/scripts
./run-sync.sh                       # build + bench all primitives
./run-sync.sh build                 # just build
./run-sync.sh bench --workers 4     # just bench, pin 4 worker threads
```

### Windows

From any terminal (`cl.exe` is auto-initialized via `vcvars64.bat`; `cmake`,
`ninja` must be on PATH):

```bat
cd benchmark\scripts
run-sync.bat
run-sync.bat bench --prims mutex,channel --langs xylem,rust
```

## Options

Same on both drivers (env vars seed defaults; CLI overrides):

| Option | Default | Meaning |
|--------|---------|---------|
| `--prims`, `-p`   | `mutex,cond,waitgroup,sem,channel` | primitives to run |
| `--langs`, `-l`   | `xylem,go,rust` | languages to compare |
| `--modes`, `-m`   | `coro,thread,mixed` | concurrency models (unsupported cells skipped) |
| `--workers`, `-w` | `0` | runtime worker threads (`0` = each runtime's default = CPU count) |
| `--repeat`, `-r`  | `3` | repeat each cell N times, report the average |
| `--permits`       | `4` | semaphore permits (the `sem` primitive only) |

`--workers` maps to the xylem scheduler worker count, Go's `GOMAXPROCS`, and
Tokio's `worker_threads`. Set it equal across runs for an apples-to-apples
comparison.

## Workload Model

All three implementations do the same logical work. `T` = `--tasks`,
`N` = `--iters`, `K` = `--permits`.

| Primitive | What each cell does | total_ops |
|-----------|---------------------|-----------|
| mutex     | `T` tasks each loop `N`x: lock / counter++ / unlock | `T*N` |
| cond      | 1 producer + 1 consumer ping-pong, `N` hand-offs | `N` |
| waitgroup | `N` rounds over a **pre-spawned** pool of `T` workers; each round releases the pool and joins it (task creation is outside the timed loop) | `T*N` |
| sem       | `T` tasks each loop `N`x: acquire / release, `K` permits | `T*N` |
| channel   | `T` senders each send `N` messages to 1 receiver | `T*N` |

The per-primitive `tasks`/`iters` defaults baked into the drivers are sized so
each cell runs roughly 1–2 seconds.

## Output

Each `(primitive, language, run)` writes `out/results/<ts>/sync-<prim>-<lang>-r<run>.json`:

```json
{
  "primitive": "mutex",
  "lang": "xylem",
  "workers": 4,
  "tasks": 8,
  "iters": 1000000,
  "total_ops": 8000000,
  "duration_sec": 1.234567,
  "ops_per_sec": 6480000,
  "ns_per_op": 154.32
}
```

The driver also prints a per-primitive comparison table (avg ops/sec, ns/op).

## Fairness & Caveats

- All C is built `-O3 -DNDEBUG -flto` (MSVC `/O2 /DNDEBUG`) and stripped; Go
  with `-ldflags="-s -w"`; Rust with `opt-level=3, lto=true`.
- The mapping is "closest idiomatic equivalent", not a byte-identical port:
  - **cond** — Tokio async has no condition variable; the Rust column uses a
    pair of `Notify` objects for the producer/consumer hand-off. Go uses a real
    `sync.Cond`.
  - **waitgroup** — measures the primitive in isolation: a fixed pool of
    `T` workers is spawned **once, outside the timed region**, and loops
    over the rounds, so task-creation cost never enters the number. Each
    round releases the pool and joins it through a fresh pair of single-use
    sync objects (a `gate` to start the round, a `fin` to join it). xylem
    uses `xylem_waitgroup`, Go uses `sync.WaitGroup`; Tokio/Rust has no
    WaitGroup, so it builds the identical gate/fin handoff from
    `Semaphore` (coro) / a `Mutex`+`Condvar` semaphore (thread).
  - **sem** — Go has no semaphore in the standard library, so it uses the
    idiomatic buffered-channel token bucket. Rust uses `tokio::sync::Semaphore`.
  - **channel** — xylem's channel is an unbounded MPSC (`create(0)`); Rust
    matches it with `mpsc::unbounded_channel`. Go has no unbounded channel, so
    it uses a buffered channel (cap 1024) and senders block on a full buffer
    (backpressure that the other two do not apply).
- Numbers are only comparable **within the same platform and run**, never
  across machines or OSes.
