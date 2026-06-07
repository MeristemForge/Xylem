# Xylem Benchmark Suite

Echo server benchmark comparing Xylem against popular networking libraries across multiple protocols.

## Competitors

| Protocol | Xylem | libuv | Boost.Asio | Go | Rust (Tokio) |
|----------|:-----:|:-----:|:----------:|:--:|:------------:|
| TCP      | x     | x     | x          | x  | x            |
| UDP      | x     | x     | x          | x  | x            |
| TLS      | x     | x     | x          | x  | x            |
| DTLS     | x     | -     | -          | x  | x            |
| RUDP     | x     | -     | -          | -  | -            |

## Runner Scripts

Two platform-specific drivers live in `benchmark/scripts/`:

| Script | Platform | Toolchain |
|--------|----------|-----------|
| `run-unix.sh` | Linux + macOS | GCC/Clang, auto-detected via `uname` |
| `run-win.bat` | Windows | MSVC (`cl.exe`), run from a VS Developer Command Prompt |

Each script exposes the same subcommands: `install`, `build`, `bench`, `all`
(default), and the same bench options.

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
run-win.bat build      :: build xylem + servers + native Win32 (IOCP) bench client
run-win.bat bench      :: run benchmarks, write results\<timestamp>\
```

## Usage

```bash
# Custom parameters (same options on both scripts):
./run-unix.sh bench --conns 10000 --duration 60
./run-unix.sh bench --servers xylem,go,rust --payload 64,4096 --mode st
./run-unix.sh bench -s xylem,rust -c 1000,5000 -d 15 --repeat 3

# Environment variables seed defaults (CLI overrides them):
REPEAT=5 DURATION=5 CONNS=1000 ./run-unix.sh bench
```

Bench options: `--servers` (xylem,libuv,boost,go,rust), `--conns`, `--payload`,
`--duration`, `--mode` (st|mt|both), `--repeat`, `--no-connrate`.

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
  - TLS/DTLS servers auto-generate self-signed certificates at startup
  - No logging in hot path for any implementation
- **Client**: Raw epoll-based C client (independent of server library)
  - Exception: RUDP client uses Xylem API (proprietary protocol)

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
  tcp/udp/tls/dtls/rudp/     protocol directories
    server/                   echo server implementations
    client/                   bench client
  scripts/
    run-unix.sh               Linux/macOS driver (install/build/bench)
    run-win.bat               Windows driver (install/build/bench)
  results/                    benchmark outputs
  bin/                        compiled binaries (gitignored)
```
