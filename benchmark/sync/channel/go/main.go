package main

import (
	"fmt"
	"sync"
	"sync/atomic"
	"time"
)

const benchDuration = 5 * time.Second

func printResult(total uint64, elapsed time.Duration) {
	sec := elapsed.Seconds()
	ops := 0.0
	nspo := 0.0
	if sec > 0 {
		ops = float64(total) / sec
	}
	if total > 0 {
		nspo = float64(elapsed.Nanoseconds()) / float64(total)
	}

	fmt.Println("{")
	fmt.Println(`  "primitive": "channel",`)
	fmt.Println(`  "lang": "go",`)
	fmt.Println(`  "mode": "cc",`)
	fmt.Printf("  \"duration_ms\": %d,\n", benchDuration.Milliseconds())
	fmt.Printf("  \"total_ops\": %d,\n", total)
	fmt.Printf("  \"duration_sec\": %.6f,\n", sec)
	fmt.Printf("  \"ops_per_sec\": %.0f,\n", ops)
	fmt.Printf("  \"ns_per_op\": %.2f\n", nspo)
	fmt.Println("}")
}

func runCC() {
	payload := 1
	ch := make(chan *int, 1024)
	done := make(chan struct{})
	var sendWG sync.WaitGroup
	var recvWG sync.WaitGroup
	var running atomic.Bool
	var counter atomic.Uint64

	running.Store(true)
	sendWG.Add(1)
	recvWG.Add(1)
	start := time.Now()

	go func() {
		defer sendWG.Done()
		for {
			select {
			case ch <- &payload:
			case <-done:
				return
			}
		}
	}()

	go func() {
		defer recvWG.Done()
		for msg := range ch {
			if msg == nil {
				continue
			}
			if !running.Load() {
				return
			}
			counter.Add(1)
		}
	}()

	time.Sleep(benchDuration)
	elapsed := time.Since(start)
	running.Store(false)
	total := counter.Load()
	close(done)
	sendWG.Wait()
	close(ch)
	recvWG.Wait()

	printResult(total, elapsed)
}

func main() {
	runCC()
}
