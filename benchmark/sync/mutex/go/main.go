package main

import (
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
	"time"
)

const benchDuration = 5 * time.Second

func runCC() {
	workers := runtime.NumCPU()
	if workers < 1 {
		workers = 4
	}
	tasks := workers * 2
	if tasks < 2 {
		tasks = 2
	}

	var mu sync.Mutex
	var wg sync.WaitGroup
	var running atomic.Bool
	var counter uint64

	wg.Add(tasks)
	start := time.Now()
	running.Store(true)

	for i := 0; i < tasks; i++ {
		go func() {
			defer wg.Done()
			for {
				mu.Lock()
				if !running.Load() {
					mu.Unlock()
					return
				}
				counter++
				mu.Unlock()
			}
		}()
	}

	time.Sleep(benchDuration)
	running.Store(false)
	wg.Wait()

	elapsed := time.Since(start)
	sec := elapsed.Seconds()
	ops := 0.0
	nspo := 0.0
	if sec > 0 {
		ops = float64(counter) / sec
	}
	if counter > 0 {
		nspo = float64(elapsed.Nanoseconds()) / float64(counter)
	}

	fmt.Println("{")
	fmt.Println(`  "primitive": "mutex",`)
	fmt.Println(`  "lang": "go",`)
	fmt.Println(`  "mode": "cc",`)
	fmt.Printf("  \"workers\": %d,\n", workers)
	fmt.Printf("  \"tasks\": %d,\n", tasks)
	fmt.Printf("  \"duration_ms\": %d,\n", benchDuration.Milliseconds())
	fmt.Printf("  \"total_ops\": %d,\n", counter)
	fmt.Printf("  \"duration_sec\": %.6f,\n", sec)
	fmt.Printf("  \"ops_per_sec\": %.0f,\n", ops)
	fmt.Printf("  \"ns_per_op\": %.2f\n", nspo)
	fmt.Println("}")
}

func main() {
	runCC()
}
