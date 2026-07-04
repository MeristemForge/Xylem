package main

import (
	"fmt"
	"sync"
	"time"
)

const benchDuration = 5 * time.Second

type state struct {
	running bool
	turn    int
	counter uint64
}

func peer(cond *sync.Cond, st *state, me int, wg *sync.WaitGroup) {
	defer wg.Done()
	other := 1 - me

	for {
		cond.L.Lock()
		for st.turn != me && st.running {
			cond.Wait()
		}
		if !st.running {
			st.turn = other
			cond.Signal()
			cond.L.Unlock()
			return
		}
		st.counter++
		st.turn = other
		cond.Signal()
		cond.L.Unlock()
	}
}

func runCC() {
	var mu sync.Mutex
	var wg sync.WaitGroup
	st := state{
		running: true,
		turn:    0,
	}
	cond := sync.NewCond(&mu)

	wg.Add(2)
	start := time.Now()
	go peer(cond, &st, 0, &wg)
	go peer(cond, &st, 1, &wg)

	time.Sleep(benchDuration)

	cond.L.Lock()
	st.running = false
	cond.Broadcast()
	cond.L.Unlock()

	wg.Wait()
	elapsed := time.Since(start)
	sec := elapsed.Seconds()
	ops := 0.0
	nspo := 0.0
	if sec > 0 {
		ops = float64(st.counter) / sec
	}
	if st.counter > 0 {
		nspo = float64(elapsed.Nanoseconds()) / float64(st.counter)
	}

	fmt.Println("{")
	fmt.Println(`  "primitive": "cond",`)
	fmt.Println(`  "lang": "go",`)
	fmt.Println(`  "mode": "cc",`)
	fmt.Printf("  \"duration_ms\": %d,\n", benchDuration.Milliseconds())
	fmt.Printf("  \"total_ops\": %d,\n", st.counter)
	fmt.Printf("  \"duration_sec\": %.6f,\n", sec)
	fmt.Printf("  \"ops_per_sec\": %.0f,\n", ops)
	fmt.Printf("  \"ns_per_op\": %.2f\n", nspo)
	fmt.Println("}")
}

func main() {
	runCC()
}
