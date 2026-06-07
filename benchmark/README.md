# Xylem Benchmark Suite

Echo server benchmark comparing Xylem against popular networking libraries across multiple protocols.

## Competitors

| Protocol | Xylem | libuv | Boost.Asio | Go | Rust (Tokio) | Modes |
|----------|:-----:|:-----:|:----------:|:--:|:------------:|:-----:|
| TCP      | x     | x     | x          | x  | x            | ST + MT |
| UDP      | x     | x     | x          | x  | x            | ST only |
| TLS      | x     | x     | x          | x  | x            | ST + MT |

UDP is **ST only**: the public UDP API exposes no `SO_REUSEPORT`, so a single
bound port cannot be fanned across worker threads the way the TCP/TLS MT
servers are. The other libraries scale UDP with `SO_REUSEPORT` at the raw
socket level, which is not a comparison Xylem can join through its public API.

## Runner Scripts

Two platform-specific drivers live in `benchmark/scripts/`:

| Script | Platform | Toolchain |
|--------|----------|-----------|
| `run-unix.sh` | Linux + macOS | GCC/Clang, auto-detected via `uname` |
| `run-win.bat` | Windows | MSVC (`cl.exe`), run from a VS Developer Command Prompt |

Each script exposes the same subcommands: `install`, `build`, `bench`, `all`
(default), and the same options. The protocol(s) under test are selected with
`--proto` (default `tcp`); `build` compiles and `bench` runs each protocol in
the comma-separated list.

```bash
./run-unix.sh build --proto tcp,udp,tls   # build all three suites
./run-unix.sh bench --proto tls           # bench just TLS
```

When `tls` is among the protocols, xylem is built with
`-DXYLEM_ENABLE_TLS=ON` and the servers/clients link OpenSSL.

## Quick Start

### Linux / macOS

```bash
cd benchmark/scripts

# One command to install deps + build + run all benchmarks:
./run-unix.sh

# Or step by step:
./run-unix.sh install   # install dependencies (Linux: apt + source; macOS: brew)
./run-unix.sh build     # compile all binaries (Release, stripped)
./run-unix.sh bench     # run benchmarks, write results/<timestamp>/
```

### Windows

Launch a "Developer Command Prompt for VS 2022" (so `cl.exe` and `cmake` are
on `PATH` — see `docs/build.md`), then:

```bat
cd benchmark\scripts

run-win.bat install    :: print winget/vcpkg setup guidance, verify cl.exe
run-win.bat build --proto tcp,udp,tls   :: build servers + native Win32 (IOCP) clients
run-win.bat bench --proto tcp           :: run benchmarks, write results\<timestamp>\
```

(TLS on Windows needs OpenSSL via vcpkg; libuv/boost servers are off by
default and also require vcpkg.)

## Usage

```bash
# Custom parameters (same options on both scripts):
./run-unix.sh bench --proto tcp,udp,tls --conns 10000 --duration 60
./run-unix.sh bench --proto tls --servers xylem,go,rust --payload 64,4096 --mode st
./run-unix.sh bench -P udp -s xylem,rust -c 1000,5000 -d 15 --repeat 3

# Environment variables seed defaults (CLI overrides them):
PROTO=tls REPEAT=5 DURATION=5 CONNS=1000 ./run-unix.sh bench
```

Bench options: `--proto` (tcp,udp,tls), `--servers` (xylem,libuv,boost,go,rust),
`--conns`, `--payload`, `--duration`, `--mode` (st|mt|both), `--repeat`,
`--no-connrate`. UDP ignores `--mode mt` (no MT row) and connrate (it is
connectionless); TLS connrate measures full TLS handshakes per second.

### Platform notes

- **macOS** uses kqueue and lacks `SO_REUSEPORT` / `/proc`; per-CPU usage
  sampling is Linux-only. The default server set narrows to `xylem,go,rust`.
- **Windows** uses wepoll (IOCP-backed); same omissions apply. libuv/boost
  servers require vcpkg and are off by default.
- Numbers are only comparable within the same platform, not across platforms.

## Methodology

- **Test pattern**: Ping-pong echo with 64-byte payloads
- **Metrics**: Throughput (msg/sec), latency (P50/P99/max), memory (RSS)
- **Fairness**:
  - All C libraries built from latest source with `-O3 -DNDEBUG -flto`
  - All binaries stripped
  - Single-threaded event loops (Go: `GOMAXPROCS=1`, Rust: `current_thread`)
  - TLS servers auto-generate self-signed certificates at startup
  - No logging in hot path for any implementation
- **Client**: Raw C load generator independent of any server library —
  epoll (Linux) / kqueue (macOS) in `<proto>-bench-unix.c`, IOCP in
  `<proto>-bench-win.c`. Modes: `throughput`, `memory`, and `connrate`
  (TCP/TLS only; TLS connrate = full handshakes/sec).

## Output Format

Each run produces JSON files in `results/<timestamp>/`:

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
  tcp/udp/tls/                protocol directories
    server/                   echo servers: <family>-echo.c[pp] (ST) and
                              <family>-echo-mt.c[pp] (MT, TCP/TLS only)
    client/                   load generator: <proto>-bench-unix.c (epoll/
                              kqueue) and <proto>-bench-win.c (IOCP)
  scripts/
    run-unix.sh               Linux/macOS driver (install/build/bench)
    run-win.bat               Windows driver (install/build/bench)
  results/                    benchmark outputs (prefixed <proto>-...)
  bin/                        compiled binaries (gitignored), <proto>-<family>-echo[-mt]
```

Binaries and result files are namespaced by protocol: e.g. `tcp-xylem-echo`,
`tls-xylem-echo-mt`, `udp-bench`, and `results/<ts>/tls-throughput-st-...json`.
