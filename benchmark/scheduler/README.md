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
./run-scheduler.sh st   # spawn matrix, single-threaded
./run-scheduler.sh mt   # spawn matrix, multi-threaded
./run-scheduler.sh      # both modes
```

Windows:

```bat
cd benchmark\scripts
run-scheduler.bat st
run-scheduler.bat mt
run-scheduler.bat
```

The driver takes one mode name (`st`, `mt`) or nothing for both, and always
builds and runs in one invocation. The matrix is fixed at the top of the
script: modes `st,mt`, langs `xylem,go,rust`, 1,000,000 tasks, one run per
cell. It writes raw JSON and prints a summary under
`benchmark/out/results/<timestamp>/`. The executables themselves do not
accept workload or mode arguments.

Results are comparable only on the same machine, operating system, compiler,
runtime versions, and CPU allocation.
