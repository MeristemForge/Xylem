#!/usr/bin/env bash
# Snapshot memory composition of the xylem TLS server mid-load at c10k/64K.
set -u
cd "$(dirname "$0")/../out" || exit 1
ulimit -n 200000 2>/dev/null || true

PORT=9544
./tls-xylem-echo $PORT >/dev/null 2>&1 &
PID=$!
taskset -cp 0 $PID >/dev/null 2>&1
sleep 2

# Drive load in the background; snapshot memory partway through.
GOMAXPROCS=8 taskset -c 8-15 ./tls-bench throughput \
    -n 10000 -d 14 -s 65536 -p $PORT >/tmp/mp.json 2>/dev/null &
BPID=$!
sleep 8

echo "===== /proc/$PID/status (selected) ====="
grep -E '^(VmRSS|RssAnon|RssFile|VmStk|VmData|VmHWM):' /proc/$PID/status
echo "===== /proc/$PID/smaps_rollup ====="
cat /proc/$PID/smaps_rollup 2>/dev/null | grep -E '^(Rss|Pss|Private_Dirty|Anonymous|Shared):'
echo "===== top anon mappings by RSS (KB) ====="
# Sum per-mapping Rss, tag heap vs anon vs stack-ish, show biggest buckets.
awk '
  /^[0-9a-f]+-/ { region=$0; rss=0; next }
  /^Rss:/ { rss=$2; total+=rss
            if (region ~ /\[heap\]/) heap+=rss
            else if (region ~ /\[stack/) stk+=rss
            else if (region ~ /rw-p .* 0+ $/) anon+=rss
            else other+=rss }
  END { printf "  heap[]=%dMB  anon-rwp=%dMB  stack[]=%dMB  other=%dMB  total=%dMB\n",
        heap/1024, anon/1024, stk/1024, other/1024, total/1024 }
' /proc/$PID/smaps

wait $BPID 2>/dev/null
TP=$(grep throughput_msg_per_sec /tmp/mp.json | grep -oE '[0-9]+' | tail -1)
echo "throughput=$TP"
kill $PID 2>/dev/null; wait $PID 2>/dev/null
