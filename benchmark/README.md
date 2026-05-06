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

## Quick Start (WSL / Linux)

```bash
cd benchmark/scripts

# One command to install deps + build + run all benchmarks:
./run.sh

# Or step by step:
./install-deps.sh      # install all dependencies from source (O3+LTO)
./build.sh             # compile all binaries (Release, stripped)
./run.sh tcp           # run only TCP benchmarks
./report.sh            # generate comparison table from latest results
```

## Usage

```bash
# Run specific protocol with custom parameters:
./run.sh tcp --conns 10000 --duration 60
./run.sh tls --conns 5000 --duration 30
./run.sh all --conns 1000 --duration 30

# Generate report from a specific run:
./report.sh results/20260501-143022/
```

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
    install-deps.sh           dependency installer
    build.sh                  build all binaries
    run.sh                    run benchmarks
    report.sh                 generate comparison tables
  results/                    benchmark outputs
  bin/                        compiled binaries (gitignored)
```
