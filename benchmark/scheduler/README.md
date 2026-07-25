# Xylem Scheduler Benchmark

This suite measures spawn-and-complete throughput for one million empty tasks,
comparing Xylem coroutines, Go goroutines, and Rust Tokio tasks.

## Layout

ST and MT are separate source files and executables, following the TCP
benchmark layout:

```text
benchmark/scheduler/
  spawn/
    xylem/spawn.c, spawn-mt.c
    go/spawn/main.go, spawn-mt/main.go
    rust/src/spawn.rs, spawn_mt.rs
```

The resulting binaries are `spawn-xylem`, `spawn-xylem-mt`, `spawn-go`,
`spawn-go-mt`, `spawn-rust`, and `spawn-rust-mt`.

## Workload

Every executable has a parameterless `main` and a fixed compile-time workload
of `1,000,000` tasks. Runtime initialization happens before the timed section.
The timer starts immediately before spawning and stops after every task has
completed. The result therefore measures spawn plus task completion bookkeeping,
not an isolated function-call latency.

Each task performs no work beyond incrementing the completion count. The
program exits with an error if the completed count is not exactly one million.

## ST And MT

ST uses one worker in all three runtimes. MT uses the machine's logical CPU
count: Xylem reads its platform CPU helper, Go uses `runtime.NumCPU()`, and Rust
uses `std::thread::available_parallelism()`.

## Output

Each binary writes one JSON object:

```json
{
  "benchmark": "spawn",
  "lang": "xylem",
  "mode": "st",
  "workers": 1,
  "tasks": 1000000,
  "completed": 1000000,
  "elapsed_sec": 0.366936,
  "tasks_per_sec": 2725271,
  "ns_per_task": 366.94
}
```

## Running

POSIX:

```bash
cd benchmark/scripts
./run-scheduler.sh build
./run-scheduler.sh bench --repeat 3
```

Windows:

```bat
cd benchmark\scripts
run-scheduler.bat build
run-scheduler.bat bench --repeat 3
```

Both runners support `--langs xylem,go,rust` and `--repeat N`. They write raw
JSON and print a summary under `benchmark/out/results/<timestamp>/`. The
executables themselves do not accept workload or mode arguments.

Results are comparable only on the same machine, operating system, compiler,
runtime versions, and CPU allocation.
