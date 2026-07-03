use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use tokio::runtime::Builder;

const DURATION_MS: u64 = 5000;

fn default_workers() -> usize {
    thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(4)
        .max(1)
}

fn print_result(mode: &str, workers: usize, tasks: usize, total_ops: u64, elapsed: Duration) {
    let sec = elapsed.as_secs_f64();
    let ops = if sec > 0.0 {
        total_ops as f64 / sec
    } else {
        0.0
    };
    let nspo = if total_ops > 0 {
        elapsed.as_nanos() as f64 / total_ops as f64
    } else {
        0.0
    };

    println!("{{");
    println!("  \"primitive\": \"mutex\",");
    println!("  \"lang\": \"rust\",");
    println!("  \"mode\": \"{}\",", mode);
    println!("  \"workers\": {},", workers);
    println!("  \"tasks\": {},", tasks);
    println!("  \"duration_ms\": {},", DURATION_MS);
    println!("  \"total_ops\": {},", total_ops);
    println!("  \"duration_sec\": {:.6},", sec);
    println!("  \"ops_per_sec\": {:.0},", ops);
    println!("  \"ns_per_op\": {:.2}", nspo);
    println!("}}");
}

async fn run_cc(workers: usize, tasks: usize) {
    let mutex = Arc::new(tokio::sync::Mutex::new(0_u64));
    let running = Arc::new(AtomicBool::new(true));
    let mut handles = Vec::with_capacity(tasks);

    let start = Instant::now();
    for _ in 0..tasks {
        let mutex = mutex.clone();
        let running = running.clone();
        handles.push(tokio::spawn(async move {
            loop {
                let mut counter = mutex.lock().await;
                if !running.load(Ordering::Relaxed) {
                    break;
                }
                *counter += 1;
            }
        }));
    }

    tokio::time::sleep(Duration::from_millis(DURATION_MS)).await;
    running.store(false, Ordering::Relaxed);
    for handle in handles {
        let _ = handle.await;
    }
    let elapsed = start.elapsed();
    let total_ops = *mutex.lock().await;
    print_result("cc", workers, tasks, total_ops, elapsed);
}

fn run_tt(workers: usize, tasks: usize) {
    let mutex = Arc::new(Mutex::new(0_u64));
    let running = Arc::new(AtomicBool::new(true));
    let mut handles = Vec::with_capacity(tasks);

    let start = Instant::now();
    for _ in 0..tasks {
        let mutex = mutex.clone();
        let running = running.clone();
        handles.push(thread::spawn(move || loop {
            let mut counter = mutex.lock().unwrap();
            if !running.load(Ordering::Relaxed) {
                break;
            }
            *counter += 1;
        }));
    }

    thread::sleep(Duration::from_millis(DURATION_MS));
    running.store(false, Ordering::Relaxed);
    for handle in handles {
        let _ = handle.join();
    }
    let elapsed = start.elapsed();
    let total_ops = *mutex.lock().unwrap();
    print_result("tt", workers, tasks, total_ops, elapsed);
}

fn main() {
    let workers = default_workers();
    let tasks = (workers * 2).max(2);

    let rt = Builder::new_multi_thread()
        .worker_threads(workers)
        .enable_time()
        .build()
        .unwrap();
    rt.block_on(run_cc(workers, tasks));
    drop(rt);

    run_tt(workers, tasks);
}
