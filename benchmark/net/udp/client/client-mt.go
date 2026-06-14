// udp-bench: cross-platform UDP load generator (Go).
//
// Replaces the single-threaded C epoll/IOCP client with a goroutine-per-socket
// generator so the server is the measured bottleneck. UDP is connectionless
// and message-framed: one Read == one echoed datagram (no stream reassembly).
//
// Modes (drop-in CLI + JSON compatible with the old C client):
//
//	throughput -n conns -d sec -s payload -h host -p port
//	memory     -n conns -w hold_sec -h host -p port
//
// (UDP has no connrate mode.)
package main

import (
	"fmt"
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

const (
	// latCapPerConn bounds the latency samples kept per socket. Once full, we
	// keep a uniform random sample of the whole run via reservoir sampling
	// (Algorithm R) so percentiles reflect steady state rather than warmup.
	latCapPerConn = 8192
	maxDatagram   = 65507 // max UDP payload
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "usage: udp-bench <mode> [options]")
		fmt.Fprintln(os.Stderr, "modes:")
		fmt.Fprintln(os.Stderr, "  throughput  -n conns -d sec -s payload -h host -p port")
		fmt.Fprintln(os.Stderr, "  memory      -n conns -w hold_sec -h host -p port")
		os.Exit(1)
	}
	switch os.Args[1] {
	case "throughput":
		runThroughput(os.Args[2:])
	case "memory":
		runMemory(os.Args[2:])
	default:
		fmt.Fprintf(os.Stderr, "unknown mode: %s\n", os.Args[1])
		os.Exit(1)
	}
}

type opts struct {
	conns, duration, payload, hold, port int
	host                                 string
}

func parseOpts(args []string) opts {
	o := opts{conns: 1000, duration: 30, payload: 64, hold: 5, host: "127.0.0.1", port: 9001}
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
	if o.payload > maxDatagram {
		o.payload = maxDatagram
	}
	return o
}

func atoiDef(s string, def int) int {
	if v, err := strconv.Atoi(s); err == nil {
		return v
	}
	return def
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
		c  *net.UDPConn
		ok bool
	}
	slots := make([]slot, o.conns)
	for i := 0; i < o.conns; i++ {
		// Dial connects the UDP socket (fixes the peer); local + synchronous.
		c, err := net.Dial("udp", addr)
		if err != nil {
			continue
		}
		slots[i] = slot{c: c.(*net.UDPConn), ok: true}
	}
	established := 0
	for i := range slots {
		if slots[i].ok {
			established++
		}
	}
	fmt.Fprintf(os.Stderr, "created %d / %d udp sockets, running %ds...\n",
		established, o.conns, o.duration)

	results := make([]result, o.conns)
	start := make(chan struct{})
	// deadlineNanos is published after the start barrier so the measurement
	// window begins when workers actually start, not while goroutines spin up.
	var deadlineNanos int64

	var wg sync.WaitGroup
	for i := 0; i < o.conns; i++ {
		if !slots[i].ok {
			continue
		}
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			c := slots[idx].c
			defer c.Close()
			out := make([]byte, o.payload)
			for j := range out {
				out[j] = 'A'
			}
			buf := make([]byte, maxDatagram)
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
				// One datagram == one reply (message-framed, no reassembly).
				if _, err := c.Read(buf); err != nil {
					return // timeout at run end, or lost datagram
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
	wg.Wait()
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

func runMemory(args []string) {
	o := parseOpts(args)
	addr := net.JoinHostPort(o.host, strconv.Itoa(o.port))
	fmt.Fprintf(os.Stderr, "memory: creating %d udp sockets to %s...\n", o.conns, addr)

	conns := make([]net.Conn, 0, o.conns)
	for i := 0; i < o.conns; i++ {
		c, err := net.Dial("udp", addr)
		if err != nil {
			continue
		}
		conns = append(conns, c)
	}
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
