# Scheduler Spawn Benchmark Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add ST and MT spawn-and-complete benchmarks for 1,000,000 Xylem coroutines, Go goroutines, and Rust Tokio tasks.

**Architecture:** Six parameterless benchmark executables implement one fixed workload each and emit the same JSON schema. Two platform runners build the binaries, execute repeated comparison cells, validate results, and store raw JSON alongside the existing benchmark results.

**Tech Stack:** C11 and Xylem, Go, Rust with Tokio, Python 3 result validation, PowerShell/cmd and POSIX shell runners.

---

## File Map

- `benchmark/tools/verify_scheduler_result.py`: validate one benchmark JSON result and its expected language/mode/task count.
- `benchmark/scheduler/spawn/xylem/spawn.c`: Xylem ST workload with one scheduler worker.
- `benchmark/scheduler/spawn/xylem/spawn-mt.c`: Xylem MT workload with logical-CPU worker count.
- `benchmark/scheduler/spawn/go/go.mod`: isolated Go benchmark module.
- `benchmark/scheduler/spawn/go/spawn/main.go`: Go ST workload.
- `benchmark/scheduler/spawn/go/spawn-mt/main.go`: Go MT workload.
- `benchmark/scheduler/spawn/rust/Cargo.toml`: Tokio crate with ST and MT binary targets.
- `benchmark/scheduler/spawn/rust/Cargo.lock`: pinned Rust dependencies.
- `benchmark/scheduler/spawn/rust/src/spawn.rs`: Tokio ST workload.
- `benchmark/scheduler/spawn/rust/src/spawn_mt.rs`: Tokio MT workload.
- `benchmark/scripts/run-scheduler.sh`: POSIX build, benchmark, collection, and summary driver.
- `benchmark/scripts/run-scheduler.bat`: Windows build, benchmark, collection, and summary driver.
- `benchmark/scheduler/README.md`: scheduler suite usage and methodology.
- `benchmark/README.md`: advertise the scheduler suite and runner paths.

### Task 1: Result Contract Test

**Files:**
- Create: `benchmark/tools/verify_scheduler_result.py`

- [ ] **Step 1: Write the failing executable contract test**

Implement a Python command with positional arguments `result`, `lang`, `mode`,
and optional `--tasks` defaulting to `1000000`. Parse the JSON file and require:

```python
required = {
    "benchmark": "spawn",
    "lang": args.lang,
    "mode": args.mode,
    "tasks": args.tasks,
    "completed": args.tasks,
}
for key, expected in required.items():
    if result.get(key) != expected:
        raise SystemExit(f"{key}: expected {expected!r}, got {result.get(key)!r}")
if not isinstance(result.get("workers"), int) or result["workers"] < 1:
    raise SystemExit("workers must be a positive integer")
for key in ("elapsed_sec", "tasks_per_sec", "ns_per_task"):
    if not isinstance(result.get(key), (int, float)) or result[key] <= 0:
        raise SystemExit(f"{key} must be positive")
```

- [ ] **Step 2: Verify RED against a missing benchmark result**

Run:

```powershell
python benchmark/tools/verify_scheduler_result.py benchmark/out/spawn-xylem.json xylem st
```

Expected: non-zero exit because `benchmark/out/spawn-xylem.json` does not exist.

- [ ] **Step 3: Verify the validator itself with a fixture piped through a temporary file**

Create a temporary JSON file outside the repository containing all required
fields, run the verifier, and remove the file. Expected: exit 0. Change
`completed` to `999999` and rerun. Expected: non-zero with a completed-count
message.

### Task 2: Xylem ST And MT Programs

**Files:**
- Create: `benchmark/scheduler/spawn/xylem/spawn.c`
- Create: `benchmark/scheduler/spawn/xylem/spawn-mt.c`

- [ ] **Step 1: Add the Xylem ST program**

Use `#ifndef BENCH_TASKS` with a default of `1000000`. The parameterless
`main(void)` runs `xylem_run()` with `.workers = 1`. Inside the root coroutine,
create a wait group, add `BENCH_TASKS`, start a nanosecond monotonic timer,
spawn `_spawn_task` exactly `BENCH_TASKS` times, wait, stop the timer, and
destroy the wait group. Each child atomically increments `completed` and calls
`xylem_waitgroup_done()`. Print the common JSON schema with `mode` set to `st`.

- [ ] **Step 2: Build and validate the ST result**

Use the repository's required MSVC CMake environment, build target `xylem`,
compile `spawn.c` as Release, run it to `benchmark/out/spawn-xylem.json`, then
run:

```powershell
python benchmark/tools/verify_scheduler_result.py benchmark/out/spawn-xylem.json xylem st
```

Expected: exit 0 and `workers == 1`.

- [ ] **Step 3: Add the Xylem MT program**

Keep it a separate translation unit. Resolve workers with
`platform_info_getcpus()` and fall back to 4 when the result is below 1. Pass
that count through `xylem_opts_t.workers`, use the same completion workload,
and print `mode` as `mt`.

- [ ] **Step 4: Build and validate the MT result**

Compile `spawn-mt.c`, run it to `benchmark/out/spawn-xylem-mt.json`, and run:

```powershell
python benchmark/tools/verify_scheduler_result.py benchmark/out/spawn-xylem-mt.json xylem mt
```

Expected: exit 0 and the JSON worker count equals the platform logical CPU
count.

- [ ] **Step 5: Review all new C lines against project style**

Check license headers, include grouping, static-name prefixes, fixed-width
types, format macros, braces, comments, allocation cleanup, and warnings.

### Task 3: Go ST And MT Programs

**Files:**
- Create: `benchmark/scheduler/spawn/go/go.mod`
- Create: `benchmark/scheduler/spawn/go/spawn/main.go`
- Create: `benchmark/scheduler/spawn/go/spawn-mt/main.go`

- [ ] **Step 1: Add the Go module and ST program**

Set the module to `xylem-bench-scheduler-spawn`. In ST, declare
`const benchTasks = 1_000_000`, call `runtime.GOMAXPROCS(1)`, prepare a
`sync.WaitGroup`, start `time.Now()`, launch `benchTasks` goroutines that
atomically increment an `int64` and call `Done`, wait, and emit the common JSON
through `encoding/json`. The function signature is `func main()` with no CLI
processing.

- [ ] **Step 2: Build and validate the Go ST result**

Run `gofmt`, build the `spawn` package as `benchmark/out/spawn-go.exe`, execute
it, and validate:

```powershell
python benchmark/tools/verify_scheduler_result.py benchmark/out/spawn-go.json go st
```

- [ ] **Step 3: Add the Go MT program**

Keep it in `spawn-mt/main.go`, set `workers := runtime.NumCPU()`, fall back to 4
if needed, call `runtime.GOMAXPROCS(workers)`, and otherwise use the same fixed
workload and output schema with mode `mt`.

- [ ] **Step 4: Build and validate the Go MT result**

Run `gofmt`, build `spawn-go-mt.exe`, execute it, and validate:

```powershell
python benchmark/tools/verify_scheduler_result.py benchmark/out/spawn-go-mt.json go mt
```

### Task 4: Rust Tokio ST And MT Programs

**Files:**
- Create: `benchmark/scheduler/spawn/rust/Cargo.toml`
- Create: `benchmark/scheduler/spawn/rust/Cargo.lock`
- Create: `benchmark/scheduler/spawn/rust/src/spawn.rs`
- Create: `benchmark/scheduler/spawn/rust/src/spawn_mt.rs`

- [ ] **Step 1: Add the Tokio crate and ST program**

Define `spawn-rust` and `spawn-rust-mt` bin targets and use Tokio runtime,
sync, and macros features. In `spawn.rs`, declare `const BENCH_TASKS: usize =
1_000_000`, create a current-thread runtime, and run an async workload. Use an
`Arc<AtomicUsize>` and `Arc<Notify>`; each spawned task increments the counter
and notifies on the final completion. Start `Instant::now()` before spawning,
await completion in a count-checking loop, and print the common JSON with a
parameterless `fn main()`.

- [ ] **Step 2: Build and validate the Rust ST result**

Run `cargo fmt --check`, `cargo build --release --bin spawn-rust`, execute the
binary to JSON, and validate:

```powershell
python benchmark/tools/verify_scheduler_result.py benchmark/out/spawn-rust.json rust st
```

- [ ] **Step 3: Add the Tokio MT program**

Use `std::thread::available_parallelism()` with fallback 4, construct a
multi-thread Tokio runtime with `.worker_threads(workers).enable_all()`, and
run the same fixed task and notification workload with mode `mt`.

- [ ] **Step 4: Build and validate the Rust MT result**

Run `cargo fmt --check`, build `spawn-rust-mt`, execute it, and validate:

```powershell
python benchmark/tools/verify_scheduler_result.py benchmark/out/spawn-rust-mt.json rust mt
```

Generate and retain `Cargo.lock`.

### Task 5: Cross-Platform Scheduler Runners

**Files:**
- Create: `benchmark/scripts/run-scheduler.sh`
- Create: `benchmark/scripts/run-scheduler.bat`

- [ ] **Step 1: Add POSIX runner help and option parsing**

Implement `build`, `bench`, and `all`, defaulting to `all`. Support
`--langs xylem,go,rust` and `--repeat N`, default 3. Reject unknown options and
workload arguments. `help` must exit 0 and document the fixed 1,000,000-task
workload.

- [ ] **Step 2: Verify POSIX help**

Run `bash benchmark/scripts/run-scheduler.sh --help`. Expected: exit 0 and
output containing `build`, `bench`, `all`, `--langs`, and `--repeat`.

- [ ] **Step 3: Implement POSIX build and benchmark paths**

Follow `run-sync.sh` for compiler selection and Xylem library discovery. Build
six binaries, execute both modes for every selected language and repetition,
validate each raw JSON file with `verify_scheduler_result.py`, store files under
one timestamped result directory, and print averages for elapsed seconds,
tasks/second, and ns/task.

- [ ] **Step 4: Add Windows runner help and option parsing**

Mirror the POSIX interface in native cmd syntax. Keep parameterless benchmark
execution and the same fixed workload statement.

- [ ] **Step 5: Verify Windows help**

Run `benchmark\scripts\run-scheduler.bat --help`. Expected: exit 0 and output
containing the same commands and options as the POSIX runner.

- [ ] **Step 6: Implement Windows build and benchmark paths**

Follow `run-sync.bat` for `vswhere`, `vcvars64.bat`, mandatory MSVC selection,
library discovery, Go/Rust builds, timestamped result storage, validation, and
summary formatting. Never delete or replace unrelated output files.

### Task 6: Scheduler Benchmark Documentation

**Files:**
- Create: `benchmark/scheduler/README.md`
- Modify: `benchmark/README.md`

- [ ] **Step 1: Write the scheduler README**

Document the six-source layout, exact 1,000,000-task spawn-and-complete
workload, parameterless executables, ST/MT worker rules, JSON schema, caveat
that completion synchronization is included, and both platform quick-start
commands.

- [ ] **Step 2: Update the root benchmark README**

Add the scheduler suite to the opening description, runner table, quick-start
examples, and directory tree. Preserve all existing net and sync content.

- [ ] **Step 3: Check documentation and scripts**

Run `git diff --check` and scan for stale statements that say the repository
contains only net and sync benchmarks.

### Task 7: Full Verification

**Files:**
- Verify all files above.

- [ ] **Step 1: Run the Windows scheduler build**

Run through the required MSVC environment:

```powershell
benchmark\scripts\run-scheduler.bat build
```

Expected: six successful release builds, no C warnings, MSVC selected for C.

- [ ] **Step 2: Run all six fixed workloads**

Run:

```powershell
benchmark\scripts\run-scheduler.bat bench --repeat 1
```

Expected: six validated result files and a summary table. Every result has
`tasks == completed == 1000000`; ST workers equal 1 and MT workers equal the
logical CPU count.

- [ ] **Step 3: Run the existing C project tests**

Use the already configured required MSVC build directory to build all current
targets and run `ctest` with failure output. Expected: zero failures and no
sanitizer diagnostics.

- [ ] **Step 4: Perform final source and repository checks**

Run `cargo fmt --check`, `gofmt` verification, Python bytecode compilation of
the validator, `git diff --check`, and `git status --short`. Review every
changed line and ensure no binary or result artifact is staged.
