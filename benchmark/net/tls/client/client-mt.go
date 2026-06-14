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
//	memory     -n conns -w hold_sec -h host -p port
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

var tlsConf = &tls.Config{InsecureSkipVerify: true} // bench servers: self-signed

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: tls-bench <mode> [options]")
		fmt.Fprintln(os.Stderr, "modes:")
		fmt.Fprintln(os.Stderr, "  throughput  -n conns -d sec -s payload -h host -p port")
		fmt.Fprintln(os.Stderr, "  connrate    -c concurrency -d sec -h host -p port")
		fmt.Fprintln(os.Stderr, "  memory      -n conns -w hold_sec -h host -p port")
		os.Exit(1)
	}
	switch os.Args[1] {
	case "throughput":
		runThroughput(os.Args[2:])
	case "connrate":
		runConnrate(os.Args[2:])
	case "memory":
		runMemory(os.Args[2:])
	default:
		fmt.Fprintf(os.Stderr, "unknown mode: %s\n", os.Args[1])
		os.Exit(1)
	}
}

type opts struct {
	conns, duration, payload, concur, hold, port int
	host                                         string
}

func parseOpts(args []string) opts {
	o := opts{conns: 1000, duration: 30, payload: 64, concur: 256, hold: 5, host: "127.0.0.1", port: 9443}
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
		case "-w":
			o.hold = atoiDef(next(), o.hold)
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

	type slot struct {
		c  *tls.Conn
		ok bool
	}
	slots := make([]slot, o.conns)
	var dwg sync.WaitGroup
	sem := make(chan struct{}, 512) // bound concurrent handshakes
	for i := 0; i < o.conns; i++ {
		dwg.Add(1)
		sem <- struct{}{}
		go func(idx int) {
			defer dwg.Done()
			defer func() { <-sem }()
			c, err := dialTLS(addr, 15*time.Second)
			if err == nil {
				slots[idx] = slot{c: c, ok: true}
			}
		}(i)
	}
	dwg.Wait()

	established := 0
	for i := range slots {
		if slots[i].ok {
			established++
		}
	}
	fmt.Fprintf(os.Stderr, "tls connected %d / %d, running %ds...\n",
		established, o.conns, o.duration)

	results := make([]result, o.conns)
	start := make(chan struct{})
	// deadlineNanos is published after the start barrier so the measurement
	// window begins when workers actually start, not while goroutines spin up.
	var deadlineNanos int64

	var rwg sync.WaitGroup
	for i := 0; i < o.conns; i++ {
		if !slots[i].ok {
			continue
		}
		rwg.Add(1)
		go func(idx int) {
			defer rwg.Done()
			c := slots[idx].c
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

	realStart := time.Now()
	atomic.StoreInt64(&deadlineNanos,
		realStart.Add(time.Duration(o.duration)*time.Second).UnixNano())
	close(start)
	rwg.Wait()
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
	fmt.Printf("{\n")
	fmt.Printf("  \"connections\": %d,\n", established)
	fmt.Printf("  \"duration_sec\": %.2f,\n", elapsed)
	fmt.Printf("  \"messages_sent\": %d,\n", totalSent)
	fmt.Printf("  \"messages_recv\": %d,\n", totalRecv)
	fmt.Printf("  \"throughput_msg_per_sec\": %.0f,\n", thr)
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

func runMemory(args []string) {
	o := parseOpts(args)
	addr := net.JoinHostPort(o.host, strconv.Itoa(o.port))
	fmt.Fprintf(os.Stderr, "memory: connecting %d tls to %s...\n", o.conns, addr)

	conns := make([]*tls.Conn, 0, o.conns)
	var mu sync.Mutex
	var wg sync.WaitGroup
	sem := make(chan struct{}, 512)
	for i := 0; i < o.conns; i++ {
		wg.Add(1)
		sem <- struct{}{}
		go func() {
			defer wg.Done()
			defer func() { <-sem }()
			c, err := dialTLS(addr, 15*time.Second)
			if err != nil {
				return
			}
			mu.Lock()
			conns = append(conns, c)
			mu.Unlock()
		}()
	}
	wg.Wait()

	established := len(conns)
	fmt.Fprintf(os.Stderr, "READY %d/%d\n", established, o.conns)
	time.Sleep(time.Duration(o.hold) * time.Second)

	fmt.Printf("{\n")
	fmt.Printf("  \"benchmark\": \"memory\",\n")
	fmt.Printf("  \"target_connections\": %d,\n", o.conns)
	fmt.Printf("  \"established_connections\": %d,\n", established)
	fmt.Printf("  \"client_rss_kb\": %d\n", rssKB())
	fmt.Printf("}\n")

	for _, c := range conns {
		c.Close()
	}
}
