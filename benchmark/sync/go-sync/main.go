// Go sync-primitive microbenchmark.
//
// Mirrors the workloads in ../xylem-sync/main.c and ../rust-sync so the
// JSON output lines up field-for-field. Each goroutine plays the role of
// a xylem coroutine.
//
//	go-sync <primitive> [--workers W] [--tasks T] [--iters N] [--permits K]
//
// Workload model:
//
//	mutex      : T goroutines each do N lock/inc/unlock        -> ops = T*N
//	cond       : 1 producer + 1 consumer, N hand-offs          -> ops = N
//	waitgroup  : N rounds, each spawns T goroutines that Done() -> ops = T*N
//	sem        : T goroutines each do N acquire/release, K slots -> ops = T*N
//	channel    : T senders each send N msgs, 1 receiver        -> ops = T*N
package main

import (
	"fmt"
	"os"
	"runtime"
	"sync"
	"time"
)

type config struct {
	prim    string
	mode    string
	workers int
	tasks   int
	iters   int
	permits int
}

func main() {
	cfg := config{mode: "coro", workers: 0, tasks: 8, iters: 100000, permits: 4}

	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}
	cfg.prim = os.Args[1]

	args := os.Args[2:]
	for i := 0; i < len(args); i++ {
		needVal := i+1 < len(args)
		switch args[i] {
		case "--mode":
			if needVal {
				cfg.mode = args[i+1]
				i++
			}
		case "--workers":
			if needVal {
				cfg.workers = atoi(args[i+1])
				i++
			}
		case "--tasks":
			if needVal {
				cfg.tasks = atoi(args[i+1])
				i++
			}
		case "--iters":
			if needVal {
				cfg.iters = atoi(args[i+1])
				i++
			}
		case "--permits":
			if needVal {
				cfg.permits = atoi(args[i+1])
				i++
			}
		default:
			usage()
			os.Exit(2)
		}
	}

	// Go has only goroutines: a "pure thread" or coroutine/thread "mixed"
	// model isn't expressible (user code always runs on a goroutine). Only
	// the coroutine mode is supported; the runner skips the rest.
	if cfg.mode != "coro" {
		fmt.Fprintf(os.Stderr, "go: mode %q unsupported (goroutines only)\n", cfg.mode)
		os.Exit(3)
	}

	if cfg.tasks < 1 {
		cfg.tasks = 1
	}
	if cfg.iters < 1 {
		cfg.iters = 1
	}
	if cfg.permits < 1 {
		cfg.permits = 1
	}

	if cfg.workers > 0 {
		runtime.GOMAXPROCS(cfg.workers)
	}

	var totalOps uint64
	var elapsed time.Duration

	switch cfg.prim {
	case "mutex":
		totalOps, elapsed = runMutex(cfg)
	case "cond":
		totalOps, elapsed = runCond(cfg)
	case "waitgroup":
		totalOps, elapsed = runWaitgroup(cfg)
	case "sem":
		totalOps, elapsed = runSem(cfg)
	case "channel":
		totalOps, elapsed = runChannel(cfg)
	default:
		usage()
		os.Exit(2)
	}

	printResult(cfg, totalOps, elapsed)
}

// ----------------------------------------------------------------- mutex

func runMutex(cfg config) (uint64, time.Duration) {
	var mu sync.Mutex
	var counter uint64
	var wg sync.WaitGroup
	wg.Add(cfg.tasks)

	t0 := time.Now()
	for t := 0; t < cfg.tasks; t++ {
		go func() {
			for i := 0; i < cfg.iters; i++ {
				mu.Lock()
				counter++
				mu.Unlock()
			}
			wg.Done()
		}()
	}
	wg.Wait()
	elapsed := time.Since(t0)

	total := uint64(cfg.tasks) * uint64(cfg.iters)
	if counter != total {
		fmt.Fprintf(os.Stderr, "mutex: counter mismatch %d != %d\n", counter, total)
	}
	return total, elapsed
}

// ------------------------------------------------------------------ cond

func runCond(cfg config) (uint64, time.Duration) {
	mu := &sync.Mutex{}
	cond := sync.NewCond(mu)
	turn := 0 // 0 = producer, 1 = consumer
	var wg sync.WaitGroup
	wg.Add(2)

	t0 := time.Now()

	go func() { // consumer
		for i := 0; i < cfg.iters; i++ {
			mu.Lock()
			for turn != 1 {
				cond.Wait()
			}
			turn = 0
			cond.Signal()
			mu.Unlock()
		}
		wg.Done()
	}()

	go func() { // producer
		for i := 0; i < cfg.iters; i++ {
			mu.Lock()
			for turn != 0 {
				cond.Wait()
			}
			turn = 1
			cond.Signal()
			mu.Unlock()
		}
		wg.Done()
	}()

	wg.Wait()
	elapsed := time.Since(t0)
	return uint64(cfg.iters), elapsed
}

// ------------------------------------------------------------- waitgroup

func runWaitgroup(cfg config) (uint64, time.Duration) {
	rounds := cfg.iters
	t0 := time.Now()
	for r := 0; r < rounds; r++ {
		var wg sync.WaitGroup
		wg.Add(cfg.tasks)
		for t := 0; t < cfg.tasks; t++ {
			go wg.Done()
		}
		wg.Wait()
	}
	elapsed := time.Since(t0)
	return uint64(rounds) * uint64(cfg.tasks), elapsed
}

// ------------------------------------------------------------------- sem

func runSem(cfg config) (uint64, time.Duration) {
	// Idiomatic Go semaphore: a buffered channel of tokens.
	sem := make(chan struct{}, cfg.permits)
	for i := 0; i < cfg.permits; i++ {
		sem <- struct{}{}
	}
	var wg sync.WaitGroup
	wg.Add(cfg.tasks)

	t0 := time.Now()
	for t := 0; t < cfg.tasks; t++ {
		go func() {
			for i := 0; i < cfg.iters; i++ {
				<-sem        // acquire
				sem <- struct{}{} // release
			}
			wg.Done()
		}()
	}
	wg.Wait()
	elapsed := time.Since(t0)
	return uint64(cfg.tasks) * uint64(cfg.iters), elapsed
}

// --------------------------------------------------------------- channel

func runChannel(cfg config) (uint64, time.Duration) {
	// MPSC: many senders, one receiver. A buffered channel approximates
	// xylem's unbounded MPSC (Go has no unbounded channel; senders block
	// on a full buffer instead).
	ch := make(chan struct{}, 1024)
	var wg sync.WaitGroup
	wg.Add(cfg.tasks)

	total := uint64(cfg.tasks) * uint64(cfg.iters)
	done := make(chan uint64, 1)

	t0 := time.Now()

	go func() { // receiver
		var got uint64
		for range ch {
			got++
		}
		done <- got
	}()

	for t := 0; t < cfg.tasks; t++ {
		go func() {
			for i := 0; i < cfg.iters; i++ {
				ch <- struct{}{}
			}
			wg.Done()
		}()
	}

	wg.Wait()
	close(ch)
	got := <-done
	elapsed := time.Since(t0)

	if got != total {
		fmt.Fprintf(os.Stderr, "channel: recv mismatch %d != %d\n", got, total)
	}
	return total, elapsed
}

// ---------------------------------------------------------------- output

func printResult(cfg config, totalOps uint64, elapsed time.Duration) {
	sec := elapsed.Seconds()
	ops := 0.0
	if sec > 0 {
		ops = float64(totalOps) / sec
	}
	nspo := 0.0
	if totalOps > 0 {
		nspo = float64(elapsed.Nanoseconds()) / float64(totalOps)
	}

	fmt.Printf("{\n")
	fmt.Printf("  \"primitive\": \"%s\",\n", cfg.prim)
	fmt.Printf("  \"lang\": \"go\",\n")
	fmt.Printf("  \"mode\": \"%s\",\n", cfg.mode)
	fmt.Printf("  \"workers\": %d,\n", cfg.workers)
	fmt.Printf("  \"tasks\": %d,\n", cfg.tasks)
	fmt.Printf("  \"iters\": %d,\n", cfg.iters)
	if cfg.prim == "sem" {
		fmt.Printf("  \"permits\": %d,\n", cfg.permits)
	}
	fmt.Printf("  \"total_ops\": %d,\n", totalOps)
	fmt.Printf("  \"duration_sec\": %.6f,\n", sec)
	fmt.Printf("  \"ops_per_sec\": %.0f,\n", ops)
	fmt.Printf("  \"ns_per_op\": %.2f\n", nspo)
	fmt.Printf("}\n")
}

func usage() {
	fmt.Fprintf(os.Stderr,
		"usage: %s <mutex|cond|waitgroup|sem|channel> "+
			"[--mode coro] [--workers W] [--tasks T] [--iters N] [--permits K]\n", os.Args[0])
}

func atoi(s string) int {
	n := 0
	neg := false
	for i, c := range s {
		if i == 0 && c == '-' {
			neg = true
			continue
		}
		if c < '0' || c > '9' {
			return n
		}
		n = n*10 + int(c-'0')
	}
	if neg {
		return -n
	}
	return n
}
