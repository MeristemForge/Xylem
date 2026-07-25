# Scheduler Spawn Benchmark Design

## Goal

Add a scheduler benchmark that compares the time required by Xylem, Go, and
Rust Tokio to spawn and complete 1,000,000 empty coroutines or tasks. The suite
must report separate single-threaded (ST) and multi-threaded (MT) results.

## Layout

The source layout mirrors the existing TCP benchmark: ST and MT are separate
source files and produce separate executables.

```text
benchmark/scheduler/
  README.md
  spawn/
    xylem/
      spawn.c
      spawn-mt.c
    go/
      go.mod
      spawn/main.go
      spawn-mt/main.go
    rust/
      Cargo.toml
      Cargo.lock
      src/spawn.rs
      src/spawn_mt.rs

benchmark/scripts/
  run-scheduler.sh
  run-scheduler.bat
```

The six executables are named `spawn-{xylem,go,rust}` for ST and
`spawn-{xylem,go,rust}-mt` for MT.

## Workload

Each executable has a parameterless `main` and runs one fixed workload:

1. Initialize the runtime and completion primitive outside the timed section.
2. Start the timer from inside the initialized runtime.
3. Spawn 1,000,000 empty tasks.
4. Wait until all tasks have completed.
5. Stop the timer and verify that the completed count is exactly 1,000,000.
6. Print one JSON result and exit successfully. A count mismatch is an error.

The workload size is a compile-time definition or constant named
`BENCH_TASKS`/`benchTasks`, defaulting to 1,000,000. Programs do not accept
command-line arguments. Any future fixed tuning values follow the same
compile-time approach.

Completion uses each runtime's normal synchronization mechanism: Xylem and Go
wait groups, and a Tokio-compatible shared completion counter and notification.
The measurement is explicitly named spawn-and-complete throughput because it
includes task execution and completion bookkeeping, not only the spawn API call.

## Worker Modes

ST and MT are separate programs for each language.

- Xylem ST sets `xylem_opts_t.workers` to 1; MT uses the logical CPU count
  reported by the existing platform information API.
- Go ST sets `GOMAXPROCS(1)`; MT sets it to `runtime.NumCPU()`.
- Rust ST uses a Tokio current-thread runtime; MT uses a Tokio multi-thread
  runtime configured with `std::thread::available_parallelism()` workers.

The selected worker count is included in every result.

## Output

Each executable writes one JSON object to standard output with these fields:

```json
{
  "benchmark": "spawn",
  "lang": "xylem",
  "mode": "st",
  "workers": 1,
  "tasks": 1000000,
  "completed": 1000000,
  "elapsed_sec": 0.123456,
  "tasks_per_sec": 8100051,
  "ns_per_task": 123.46
}
```

Timing uses each language's monotonic clock. Diagnostic failures go to standard
error; successful standard output remains machine-readable JSON.

## Runner

`run-scheduler.sh` and `run-scheduler.bat` follow the existing benchmark driver
conventions and expose `build`, `bench`, and `all` commands. They build the
Xylem library plus the selected language executables, run both ST and MT modes,
repeat each cell three times by default, and store raw JSON under
`benchmark/out/results/<timestamp>/`.

The drivers support language filtering and repeat-count options but do not pass
workload parameters to the benchmark executables. They print an averaged table
covering elapsed time, tasks per second, and nanoseconds per task.

## Documentation And Verification

`benchmark/scheduler/README.md` documents the workload, ST/MT worker selection,
output fields, runner commands, and the limitation that the result includes
completion synchronization. The root benchmark README gains the scheduler suite
and runner in its directory overview.

Verification covers all six release builds, one successful execution of each
binary with `tasks == completed == 1000000`, valid JSON output, and smoke tests
for both runner help commands. Existing project tests must remain passing.
