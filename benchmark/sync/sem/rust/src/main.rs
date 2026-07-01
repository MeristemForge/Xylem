use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::Semaphore;

const DURATION_MS: u64 = 5000;

#[tokio::main]
async fn main() {
    let sem_a = Arc::new(Semaphore::new(1));
    let sem_b = Arc::new(Semaphore::new(0));
    let running = Arc::new(AtomicBool::new(true));
    let counter = Arc::new(AtomicU64::new(0));

    let a = {
        let sem_a = sem_a.clone();
        let sem_b = sem_b.clone();
        let running = running.clone();
        let counter = counter.clone();
        tokio::spawn(async move {
            loop {
                sem_a.acquire().await.unwrap().forget();
                sem_b.add_permits(1);
                counter.fetch_add(1, Ordering::Relaxed);
                if !running.load(Ordering::Relaxed) {
                    break;
                }
            }
        })
    };

    let b = {
        let sem_a = sem_a.clone();
        let sem_b = sem_b.clone();
        let running = running.clone();
        let counter = counter.clone();
        tokio::spawn(async move {
            loop {
                sem_b.acquire().await.unwrap().forget();
                sem_a.add_permits(1);
                counter.fetch_add(1, Ordering::Relaxed);
                if !running.load(Ordering::Relaxed) {
                    break;
                }
            }
        })
    };

    let t0 = Instant::now();
    tokio::time::sleep(Duration::from_millis(DURATION_MS)).await;
    running.store(false, Ordering::Relaxed);
    sem_a.add_permits(1);
    sem_b.add_permits(1);
    let _ = tokio::join!(a, b);

    let elapsed = t0.elapsed();
    let sec = elapsed.as_secs_f64();
    let ops = if sec > 0.0 { counter.load(Ordering::Relaxed) as f64 / sec } else { 0.0 };
    let nspo = if ops > 0.0 { elapsed.as_nanos() as f64 / counter.load(Ordering::Relaxed) as f64 } else { 0.0 };

    println!("{{");
    println!("  \"primitive\": \"sem\",");
    println!("  \"lang\": \"rust\",");
    println!("  \"mode\": \"coro\",");
    println!("  \"duration_ms\": {},", DURATION_MS);
    println!("  \"total_ops\": {},", counter.load(Ordering::Relaxed));
    println!("  \"duration_sec\": {:.6},", sec);
    println!("  \"ops_per_sec\": {:.0},", ops);
    println!("  \"ns_per_op\": {:.2}", nspo);
    println!("}}");
}
