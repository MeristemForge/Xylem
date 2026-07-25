package main

import (
	"encoding/json"
	"os"
	"runtime"
	"sync"
	"sync/atomic"
	"time"
)

const benchTasks = 1_000_000

type result struct {
	Benchmark   string  `json:"benchmark"`
	Lang        string  `json:"lang"`
	Mode        string  `json:"mode"`
	Workers     int     `json:"workers"`
	Tasks       int     `json:"tasks"`
	Completed   int64   `json:"completed"`
	ElapsedSec  float64 `json:"elapsed_sec"`
	TasksPerSec float64 `json:"tasks_per_sec"`
	NsPerTask   float64 `json:"ns_per_task"`
}

func main() {
	workers := runtime.NumCPU()
	if workers < 1 {
		workers = 4
	}
	runtime.GOMAXPROCS(workers)

	var completed int64
	var wg sync.WaitGroup
	wg.Add(benchTasks)

	started := time.Now()
	for i := 0; i < benchTasks; i++ {
		go func() {
			atomic.AddInt64(&completed, 1)
			wg.Done()
		}()
	}
	wg.Wait()
	elapsed := time.Since(started)

	count := atomic.LoadInt64(&completed)
	seconds := elapsed.Seconds()
	benchResult := result{
		Benchmark:   "spawn",
		Lang:        "go",
		Mode:        "mt",
		Workers:     workers,
		Tasks:       benchTasks,
		Completed:   count,
		ElapsedSec:  seconds,
		TasksPerSec: float64(benchTasks) / seconds,
		NsPerTask:   float64(elapsed.Nanoseconds()) / float64(benchTasks),
	}
	if err := json.NewEncoder(os.Stdout).Encode(benchResult); err != nil {
		os.Exit(1)
	}
	if count != benchTasks {
		os.Exit(1)
	}
}
