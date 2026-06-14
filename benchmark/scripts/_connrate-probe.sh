#!/usr/bin/env bash
# Compare TLS connrate (handshakes/sec) after lazy rbuf/wbuf allocation.
set -u
cd "$(dirname "$0")/../out" || exit 1
ulimit -n 200000 2>/dev/null || true

declare -A BIN=( [xylem]=tls-xylem-echo [go]=tls-go-echo [rust]=tls-rust-echo )
PORT=9555
for CONC in 1000 10000; do
    echo "=== connrate concurrency=$CONC ==="
    printf "%-8s %12s %8s\n" "server" "conn/s" "fails"
    for name in xylem go rust; do
        b=./${BIN[$name]}
        [ -x "$b" ] || { echo "$name: missing"; continue; }
        taskset -c 0 "$b" $PORT >/dev/null 2>&1 &
        PID=$!
        sleep 2
        GOMAXPROCS=8 taskset -c 8-15 ./tls-bench connrate \
            -c $CONC -d 10 -p $PORT >/tmp/cr.json 2>/dev/null
        CPS=$(grep connects_per_sec /tmp/cr.json | grep -oE '[0-9]+' | tail -1)
        F=$(grep failed_connects /tmp/cr.json | grep -oE '[0-9]+' | tail -1)
        kill $PID 2>/dev/null; wait $PID 2>/dev/null; sleep 1
        printf "%-8s %12s %8s\n" "$name" "${CPS:-NA}" "${F:-0}"
    done
done
