// tls-bench: cross-platform TLS-over-TCP load generator (Go).
//
// Replaces the C/OpenSSL epoll/IOCP client. Goroutine-per-connection so the
// server is the measured bottleneck. Uses the standard crypto/tls stack with
// InsecureSkipVerify (the bench servers present self-signed certs).
//
// Modes (drop-in CLI + JSON compatible with the old C client):
//
//	throughput -n conns -d sec -s payload -h host -p port
//	connrate   -c concurrency -d sec -h host -p port   (full TLS handshakes/sec)
package main

import (
	"crypto/tls"
	"fmt"
	"io"
	"math/rand"
	"net"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// latCapPerConn bounds the latency samples kept per connection. Once full, we
// keep a uniform random sample of the whole run via reservoir sampling
// (Algorithm R) so percentiles reflect steady state rather than warmup.
const latCapPerConn = 8192
const warmupRounds = 5
const warmupTimeout = 30 * time.Second

var tlsConf = &tls.Config{InsecureSkipVerify: true} // bench servers: self-signed

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: tls-bench <mode> [options]")
		fmt.Fprintln(os.Stderr, "modes:")
		fmt.Fprintln(os.Stderr, "  throughput  -n conns -d sec -s payload -h host -p port")
		fmt.Fprintln(os.Stderr, "  connrate    -c concurrency -d sec -h host -p port")
		os.Exit(1)
	}
	switch os.Args[1] {
	case "throughput":
		runThroughput(os.Args[2:])
	case "connrate":
		runConnrate(os.Args[2:])
	default:
		fmt.Fprintf(os.Stderr, "unknown mode: %s\n", os.Args[1])
		os.Exit(1)
	}
}

type opts struct {
	conns, duration, payload, concur, port int
	host                                         string
}

func parseOpts(args []string) opts {
	o := opts{conns: 1000, duration: 30, payload: 64, concur: 256, host: "127.0.0.1", port: 9443}
	for i := 0; i < len(args); i++ {
		next := func() string {
			if i+1 < len(args) {
				i++
				return args[i]
			}
			return ""
		}
		switch args[i] {
		case "-n":
			o.conns = atoiDef(next(), o.conns)
		case "-d":
			o.duration = atoiDef(next(), o.duration)
		case "-s":
			o.payload = atoiDef(next(), o.payload)
		case "-c":
			o.concur = atoiDef(next(), o.concur)
		case "-h":
			o.host = next()
		case "-p":
			o.port = atoiDef(next(), o.port)
		}
	}
	if o.payload < 1 {
		o.payload = 1
	}
	if o.payload > 65536 {
		o.payload = 65536
	}
	return o
}

func atoiDef(s string, def int) int {
	if v, err := strconv.Atoi(s); err == nil {
		return v
	}
	return def
}

// dialTLS opens a TCP connection (TCP_NODELAY), wraps it in TLS, and completes
// the handshake before returning.
func dialTLS(addr string, timeout time.Duration) (*tls.Conn, error) {
	raw, err := (&net.Dialer{Timeout: timeout}).Dial("tcp", addr)
	if err != nil {
		return nil, err
	}
	if tc, ok := raw.(*net.TCPConn); ok {
		_ = tc.SetNoDelay(true)
	}
	c := tls.Client(raw, tlsConf)
	if err := c.Handshake(); err != nil {
		raw.Close()
		return nil, err
	}
	return c, nil
}

func establishTLSConnections(addr string, target int) []*tls.Conn {
	if target <= 0 {
		return nil
	}

	conns := make([]*tls.Conn, target)
	var wg sync.WaitGroup
	sem := make(chan struct{}, 512) // bound concurrent handshakes

	for i := 0; i < target; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			for {
				sem <- struct{}{}
				c, err := dialTLS(addr, 15*time.Second)
				<-sem
				if err == nil {
					conns[idx] = c
					return
				}
				time.Sleep(10 * time.Millisecond)
			}
		}(i)
	}
	wg.Wait()
	return conns
}

func rssKB() int64 {
	data, err := os.ReadFile("/proc/self/status")
	if err != nil {
		return 0
	}
	for _, line := range strings.Split(string(data), "\n") {
		if strings.HasPrefix(line, "VmRSS:") {
			f := strings.Fields(line)
			if len(f) >= 2 {
				v, _ := strconv.ParseInt(f[1], 10, 64)
				return v
			}
		}
	}
	return 0
}

func snapshotProcStatFromEnv(name string) {
	path := os.Getenv(name)
	if path == "" {
		return
	}
	data, err := os.ReadFile("/proc/stat")
	if err != nil {
		return
	}
	lines := strings.Split(string(data), "\n")
	out := make([]byte, 0, len(data))
	for _, line := range lines {
		if strings.HasPrefix(line, "cpu") && len(line) > 3 && line[3] >= '0' && line[3] <= '9' {
			out = append(out, line...)
			out = append(out, '\n')
		}
	}
	_ = os.WriteFile(path, out, 0644)
}

func touchFileFromEnv(name string) {
	path := os.Getenv(name)
	if path == "" {
		return
	}
	_ = os.WriteFile(path, []byte("1\n"), 0644)
}

func pctile(sorted []int64, p float64) int64 {
	if len(sorted) == 0 {
		return 0
	}
	idx := int(p * float64(len(sorted)))
	if idx >= len(sorted) {
		idx = len(sorted) - 1
	}
	return sorted[idx]
}

type result struct {
	sent uint64
	recv uint64
	lats []int64 // reservoir sample of round-trip latencies (microseconds)
}

func runThroughput(args []string) {
	o := parseOpts(args)
	addr := net.JoinHostPort(o.host, strconv.Itoa(o.port))

	// establishTLSConnections retries until every dial succeeds, so all
	// connections are always usable.
	conns := establishTLSConnections(addr, o.conns)
	established := len(conns)
	fmt.Fprintf(os.Stderr, "tls connected %d / %d, running %ds...\n",
		established, o.conns, o.duration)

	results := make([]result, o.conns)
	start := make(chan struct{})
	// deadlineNanos is published after the start barrier so the measurement
	// window begins when workers actually start, not while goroutines spin up.
	var deadlineNanos int64

	var rwg sync.WaitGroup
	var warmwg sync.WaitGroup
	for i := 0; i < o.conns; i++ {
		rwg.Add(1)
		if warmupRounds > 0 {
			warmwg.Add(1)
		}
		go func(idx int) {
			defer rwg.Done()
			c := conns[idx]
			defer c.Close()
			out := make([]byte, o.payload)
			for j := range out {
				out[j] = 'A'
			}
			in := make([]byte, o.payload)
			res := &results[idx]
			res.lats = make([]int64, 0, latCapPerConn)
			rng := rand.New(rand.NewSource(time.Now().UnixNano() + int64(idx)))
			var latCount int64
			if warmupRounds > 0 {
				ok := true
				_ = c.SetDeadline(time.Now().Add(warmupTimeout))
				for r := 0; r < warmupRounds; r++ {
					if _, err := c.Write(out); err != nil {
						ok = false
						break
					}
					if _, err := io.ReadFull(c, in); err != nil {
						ok = false
						break
					}
				}
				_ = c.SetDeadline(time.Time{})
				warmwg.Done()
				if !ok {
					return
				}
			}
			<-start
			deadline := time.Unix(0, atomic.LoadInt64(&deadlineNanos))
			_ = c.SetDeadline(deadline)
			for time.Now().Before(deadline) {
				ts := time.Now()
				if _, err := c.Write(out); err != nil {
					return
				}
				res.sent++
				// One full payload per round-trip (byte-accurate framing:
				// a >16 KiB reply spans multiple TLS records / reads).
				if _, err := io.ReadFull(c, in); err != nil {
					return
				}
				res.recv++
				us := time.Since(ts).Microseconds()
				latCount++
				if len(res.lats) < latCapPerConn {
					res.lats = append(res.lats, us)
				} else if k := rng.Int63n(latCount); k < latCapPerConn {
					res.lats[k] = us // reservoir replacement
				}
			}
		}(i)
	}

	warmwg.Wait()
	touchFileFromEnv("BENCH_WINDOW_START_FILE")
	snapshotProcStatFromEnv("BENCH_CPU_BEFORE_FILE")
	realStart := time.Now()
	atomic.StoreInt64(&deadlineNanos,
		realStart.Add(time.Duration(o.duration)*time.Second).UnixNano())
	close(start)
	rwg.Wait()
	snapshotProcStatFromEnv("BENCH_CPU_AFTER_FILE")
	touchFileFromEnv("BENCH_WINDOW_END_FILE")
	elapsed := time.Since(realStart).Seconds()

	var totalSent, totalRecv uint64
	allLats := make([]int64, 0, 1<<20)
	for i := range results {
		totalSent += results[i].sent
		totalRecv += results[i].recv
		allLats = append(allLats, results[i].lats...)
	}
	sort.Slice(allLats, func(a, b int) bool { return allLats[a] < allLats[b] })

	thr := 0.0
	if elapsed > 0 {
		thr = float64(totalRecv) / elapsed
	}
	// Per-connection throughput normalizes out differences in the established
	// connection count, giving a fair single-stream comparison across servers.
	thrPerConn := 0.0
	if established > 0 {
		thrPerConn = thr / float64(established)
	}
	fmt.Printf("{\n")
	fmt.Printf("  \"connections\": %d,\n", established)
	fmt.Printf("  \"target_connections\": %d,\n", o.conns)
	fmt.Printf("  \"duration_sec\": %.2f,\n", elapsed)
	fmt.Printf("  \"messages_sent\": %d,\n", totalSent)
	fmt.Printf("  \"messages_recv\": %d,\n", totalRecv)
	fmt.Printf("  \"throughput_msg_per_sec\": %.0f,\n", thr)
	fmt.Printf("  \"throughput_msg_per_sec_per_conn\": %.2f,\n", thrPerConn)
	fmt.Printf("  \"latency_p50_us\": %d,\n", pctile(allLats, 0.50))
	fmt.Printf("  \"latency_p99_us\": %d,\n", pctile(allLats, 0.99))
	fmt.Printf("  \"latency_max_us\": %d,\n", pctile(allLats, 1.0))
	fmt.Printf("  \"memory_rss_kb\": %d\n", rssKB())
	fmt.Printf("}\n")
}

// connrate: measure full TLS handshakes per second.
func runConnrate(args []string) {
	o := parseOpts(args)
	addr := net.JoinHostPort(o.host, strconv.Itoa(o.port))
	fmt.Fprintf(os.Stderr, "connrate: %d concurrent tls handshakes to %s for %ds\n",
		o.concur, addr, o.duration)

	var ok, fail uint64
	deadline := time.Now().Add(time.Duration(o.duration) * time.Second)

	var wg sync.WaitGroup
	realStart := time.Now()
	for i := 0; i < o.concur; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for time.Now().Before(deadline) {
				c, err := dialTLS(addr, 10*time.Second)
				if err != nil {
					atomic.AddUint64(&fail, 1)
					continue
				}
				atomic.AddUint64(&ok, 1)
				if tc, isTCP := c.NetConn().(*net.TCPConn); isTCP {
					_ = tc.SetLinger(0) // RST close; avoid TIME_WAIT buildup
				}
				c.Close()
			}
		}()
	}
	wg.Wait()
	elapsed := time.Since(realStart).Seconds()

	cps := 0.0
	if elapsed > 0 {
		cps = float64(ok) / elapsed
	}
	fmt.Printf("{\n")
	fmt.Printf("  \"benchmark\": \"connrate\",\n")
	fmt.Printf("  \"duration_sec\": %.2f,\n", elapsed)
	fmt.Printf("  \"concurrency\": %d,\n", o.concur)
	fmt.Printf("  \"total_connects\": %d,\n", ok)
	fmt.Printf("  \"failed_connects\": %d,\n", fail)
	fmt.Printf("  \"connects_per_sec\": %.0f\n", cps)
	fmt.Printf("}\n")
}

