# Xylem Benchmark Suite

Benchmark suites for Xylem networking, synchronization primitives, and
scheduler task creation.

## Competitors

| Protocol | Xylem | Go | Rust (Tokio) | Modes |
|----------|:-----:|:--:|:------------:|:-----:|
| TCP      | x     | x  | x            | ST + MT |
| UDP      | x     | x  | x            | ST only |
| TLS      | x     | x  | x            | ST + MT |

All protocols compare Xylem against Go and Rust (Tokio).

UDP is **ST only**: the public UDP API exposes no `SO_REUSEPORT`, so a single
bound port cannot be fanned across worker threads the way the TCP/TLS MT
servers are. The other libraries scale UDP with `SO_REUSEPORT` at the raw
socket level, which is not a comparison Xylem can join through its public API.

## Runner Scripts

Platform-specific drivers live in `benchmark/scripts/`:

| Script | Platform | Toolchain |
|--------|----------|-----------|
| `run-net.sh` | Linux + macOS | GCC/Clang, auto-detected via `uname` |
| `run-net.bat` | Windows | MSVC (`cl.exe`), auto-initialized via `vcvars64.bat` |
| `run-scheduler.sh` | Linux + macOS | GCC/Clang, Go, Rust |
| `run-scheduler.bat` | Windows | MSVC (`cl.exe`), Go, Rust |

The network drivers expose `install`, `build`, `bench`, and `all` (default).
The scheduler drivers expose `build`, `bench`, and `all`; their workload is
fixed at one million tasks. The network benchmark matrix is fixed in the
`NET_BENCH_*` constants inside the network scripts.

```bash
./run-net.sh build
./run-net.sh bench
```

Scheduler spawn benchmark:

```bash
./run-scheduler.sh all
```

When `tls` is among the protocols, xylem is built with
`-DXYLEM_ENABLE_TLS=ON` and the servers/clients link OpenSSL.

## Quick Start

### Linux / macOS

```bash
cd benchmark/scripts

# One command to install deps + build + run all benchmarks:
./run-net.sh

# Or step by step:
./run-net.sh install   # install dependencies (Linux: apt + source; macOS: brew)
./run-net.sh build     # compile all binaries (Release, stripped)
./run-net.sh bench     # run benchmarks, write out/results/<timestamp>/
```

### Windows

From any terminal (`cmake` and `ninja` on `PATH` — install via winget). The
build step auto-detects Visual Studio and initializes MSVC (`vcvars64.bat`),
so no "Developer Command Prompt" is required:

```bat
cd benchmark\scripts

run-net.bat install    :: print winget/vcpkg setup guidance, verify cl.exe
run-net.bat build      :: build servers + native Win32 (IOCP) clients
run-net.bat bench      :: run benchmarks, write out\results\<timestamp>\
```

(TLS on Windows needs OpenSSL via vcpkg. The Windows driver builds only the
xylem/go/rust families.)

## Usage

```bash
./run-net.sh build
./run-net.sh bench
./run-net.sh all
```

The net driver uses a fixed matrix: `tcp,udp,tls`, `xylem,go,rust`,
connections `1000,10000`, payloads `64,4096,65536`, duration `10s`, and
ST+MT where the protocol supports it. Edit the `NET_BENCH_*` constants in
`run-net.sh` / `run-net.bat` to change the standard suite.

### Platform notes

- **macOS** uses kqueue and lacks `SO_REUSEPORT` / `/proc`; per-CPU usage
  sampling is Linux-only.
- **Windows** uses wepoll (IOCP-backed); same omissions apply. The driver
  builds only the xylem/go/rust families.
- Numbers are only comparable within the same platform, not across platforms.

## Methodology

- **Test pattern**: Ping-pong echo with 64-byte payloads
- **Metrics**: Throughput (msg/sec), latency (P50/P99/max), memory (RSS)
- **Fairness**:
  - Xylem built from source with `-O3 -DNDEBUG -flto`
  - All binaries stripped
  - Single-threaded event loops (Go: `GOMAXPROCS=1`, Rust: `current_thread`)
  - TLS servers auto-generate self-signed certificates at startup
  - No logging in hot path for any implementation
- **Client**: independent Go load generator (goroutine-per-connection, multi-core, never caps client-side load), one per protocol under `net/<proto>/client/`; the Go runtime netpoller maps onto
  epoll (Linux) / kqueue (macOS) readiness model, IOCP completion model on
  Windows. Each protocol's client is a small Go module (`client-mt.go`); TLS
  uses the standard `crypto/tls` (no OpenSSL). Modes:
  `throughput`, `memory`, and `connrate`
  (TCP/TLS only; TLS connrate = full handshakes/sec).

## Output Format

Each run produces JSON files in `out/results/<timestamp>/`:

```json
{
  "connections": 1000,
  "duration_sec": 30.00,
  "messages_sent": 1500000,
  "messages_recv": 1500000,
  "throughput_msg_per_sec": 50000,
  "latency_p50_us": 120,
  "latency_p99_us": 450,
  "latency_max_us": 2300,
  "memory_rss_kb": 8192
}
```

## Directory Structure

```
benchmark/
  net/                        network protocol suites
    tcp/udp/tls/              protocol directories
      server/                 echo servers, one directory per family:
        xylem-echo/           server.c (ST) + server-mt.c (MT, tcp/tls)
        go-echo/              one module: echo/server.go (ST) + echo-mt/server.go (MT)
        rust-echo/            one crate: src/server.rs (ST) + src/server_mt.rs (MT)
      client/                 load generator: Go module (client-mt.go),
                              file per protocol (epoll/kqueue readiness on
                              POSIX, IOCP completion on Windows)
  sync/                       sync-primitive microbenchmarks (separate suite)
  scheduler/                  scheduler spawn microbenchmarks (ST + MT)
  scripts/
    run-net.sh                Linux/macOS net driver (install/build/bench)
    run-net.bat               Windows net driver (install/build/bench)
    run-sync.sh               Linux/macOS sync driver
    run-sync.bat              Windows sync driver
    run-scheduler.sh          Linux/macOS scheduler driver
    run-scheduler.bat         Windows scheduler driver
    plot_results.py           render charts from an out/results/<ts>/ directory
  out/                        all build output (gitignored)
    <proto>-<family>-echo[-mt], <proto>-bench   compiled binaries
    build/                    xylem CMake build tree
    results/<ts>/             per-run JSON (prefixed <proto>-... or scheduler-...)
```

The xylem/go/rust families each live in a single per-family directory that
yields both the ST and MT binaries where the protocol supports it.

Binaries and result files are namespaced by protocol: e.g. `tcp-xylem-echo`,
`tls-xylem-echo-mt`, `udp-bench`, and `out/results/<ts>/tls-throughput-st-...json`.
