use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use tokio::runtime::Builder;
use tokio::sync::Notify;

const BENCH_TASKS: usize = 1_000_000;

fn default_workers() -> usize {
    thread::available_parallelism()
        .map(|count| count.get())
        .unwrap_or(4)
        .max(1)
}

async fn run() -> (usize, Duration) {
    let completed = Arc::new(AtomicUsize::new(0));
    let completion = Arc::new(Notify::new());
    let started = Instant::now();

    for _ in 0..BENCH_TASKS {
        let completed = Arc::clone(&completed);
        let completion = Arc::clone(&completion);
        tokio::spawn(async move {
            completed.fetch_add(1, Ordering::Relaxed);
            completion.notify_one();
        });
    }

    while completed.load(Ordering::Relaxed) != BENCH_TASKS {
        completion.notified().await;
    }

    (completed.load(Ordering::Relaxed), started.elapsed())
}

fn print_result(workers: usize, completed: usize, elapsed: Duration) {
    let elapsed_sec = elapsed.as_secs_f64();
    let tasks_per_sec = BENCH_TASKS as f64 / elapsed_sec;
    let ns_per_task = elapsed.as_nanos() as f64 / BENCH_TASKS as f64;

    println!("{{");
    println!("  \"benchmark\": \"spawn\",");
    println!("  \"lang\": \"rust\",");
    println!("  \"mode\": \"mt\",");
    println!("  \"workers\": {workers},");
    println!("  \"tasks\": {BENCH_TASKS},");
    println!("  \"completed\": {completed},");
    println!("  \"elapsed_sec\": {elapsed_sec:.6},");
    println!("  \"tasks_per_sec\": {tasks_per_sec:.0},");
    println!("  \"ns_per_task\": {ns_per_task:.2}");
    println!("}}");
}

fn main() {
    let workers = default_workers();
    let runtime = Builder::new_multi_thread()
        .worker_threads(workers)
        .build()
        .unwrap();
    let (completed, elapsed) = runtime.block_on(run());

    print_result(workers, completed, elapsed);
    if completed != BENCH_TASKS {
        std::process::exit(1);
    }
}
