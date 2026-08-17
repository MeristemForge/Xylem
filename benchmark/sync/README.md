# Xylem Sync-Primitive Benchmark

Microbenchmarks for Xylem synchronization primitives, compared with closest
standard Go and Rust equivalents where the language can express the same
context pairing.

Each benchmark program is self-contained: it takes no command-line parameters,
runs a fixed 5-second timed window, prints one or more JSON objects, and exits.
The driver scripts build the per-primitive binaries, run them, collect the JSON,
and print a comparison table.

## Current Suite

| Primitive | Xylem | Go | Rust |
|-----------|:-----:|:--:|:----:|
| mutex | `xylem_mutex` | `sync.Mutex` | `tokio::sync::Mutex` / `std::sync::Mutex` |
| cond | `xylem_cond` | `sync.Cond` | `std::sync::Condvar` |
| sem | `xylem_sem` | - | `tokio::sync::Semaphore` |
| channel | `xylem_channel` | buffered `chan` | `tokio::sync::mpsc` / `std::sync::mpsc` |

The current checked-in benchmark programs cover `mutex`, `cond`, `sem`, and
`channel`. Waitgroup and raw handoff probes are not part of the current
per-primitive suite.

## Modes

The Xylem programs report four pinned context pairs:

| Mode | Sender / first party | Receiver / second party |
|------|----------------------|-------------------------|
| `cc` | coroutine | coroutine |
| `tt` | OS thread | OS thread |
| `ct` | coroutine | OS thread |
| `tc` | OS thread | coroutine |

For symmetric primitives (`mutex`, `cond`, `sem`), the labels pin which party is
created in which context. For `channel`, the direction is literal: `ct` means a
coroutine sender feeding a thread receiver, and `tc` means a thread sender
feeding a coroutine receiver.

Go only reports `cc`, because user code runs in goroutines rather than raw OS
threads. Rust reports the cells its standard libraries can model:

- `mutex`: `cc` via Tokio mutex and `tt` via standard mutex.
- `cond`: `tt` via `std::sync::Condvar`.
- `sem`: `cc` via Tokio semaphore.
- `channel`: `cc`, `tt`, `ct`, and `tc`, choosing Tokio or standard MPSC based
  on the receiver context.

## Workloads

All current workloads are fixed-duration runs. `total_ops` is the number of
successful operations counted during the timed window.

| Primitive | Workload | total_ops |
|-----------|----------|-----------|
| mutex | Multiple workers loop: lock, increment, unlock | increments |
| cond | Two-party ping-pong through a condition variable and mutex | turns |
| sem | Two-party semaphore handoff | handoff steps |
| channel | One sender, one receiver, one-way, no benchmark-level backpressure | received messages |

Channel intentionally measures steady-state unbounded queue throughput, not
forced wake latency. The sender never waits for an acknowledgement, so the
receiver often drains already-buffered messages. A ping-pong or ack-gated
benchmark would measure a different path.

## Layout

```
benchmark/sync/
  mutex/{xylem,go,rust}/
  cond/{xylem,go,rust}/
  sem/{xylem,rust}/
  channel/{xylem,go,rust}/

benchmark/out/
  built binaries, CMake build tree, results/<timestamp>/*.json

benchmark/scripts/
  run-sync.sh
  run-sync.bat
```

## Quick Start

Linux / macOS:

```bash
cd benchmark/scripts
./run-sync.sh mutex     # mutex matrix: xylem vs go vs rust
./run-sync.sh channel   # channel matrix
./run-sync.sh           # all four primitives
```

Windows:

```bat
cd benchmark\scripts
run-sync.bat mutex
run-sync.bat channel
run-sync.bat
```

The driver takes one primitive name (`mutex`, `cond`, `sem`, `channel`) or
nothing for the full suite, and always builds and runs in one invocation.
The matrix is fixed at the top of the script: prims `mutex,cond,sem,channel`,
langs `xylem,go,rust`, 5s per cell, one run per cell.

## Output

Each run writes JSON under `benchmark/out/results/<timestamp>/`. Programs that
support multiple modes print multiple JSON objects; the driver extracts the
object matching the mode it is summarizing.

Example:

```json
{
  "primitive": "channel",
  "lang": "xylem",
  "mode": "tc",
  "duration_ms": 5000,
  "total_ops": 28694659,
  "duration_sec": 5.000000,
  "ops_per_sec": 5738131,
  "ns_per_op": 174.27
}
```

The driver also prints a comparison table with each cell's `ops/s`, `ns/op`,
and `total_ops`.

## Caveats

- The mapping is idiomatic, not byte-identical. Rust channel results are
  especially not one primitive across all modes: Tokio MPSC is used when the
  receiver is async, and standard MPSC is used when the receiver is a thread.
- Go channel uses a finite buffered channel, because Go has no unbounded
  standard channel. That can introduce sender blocking if the buffer fills.
- Xylem channel is one context-adaptive primitive across all four modes. Its
  send path allocates one node per message and supports runtime-aware receiver
  wakeup, so the channel benchmark includes that generality.
- Results are comparable only within the same machine, OS, compiler/runtime
  versions, and run.
