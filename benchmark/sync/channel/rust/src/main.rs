use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc;
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use tokio::runtime::{Builder, Runtime};
use tokio::sync::mpsc as tokio_mpsc;

const DURATION_MS: u64 = 5000;

fn default_workers() -> usize {
    thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(4)
        .max(1)
}

fn print_result(mode: &str, total_ops: u64, elapsed: Duration) {
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
    println!("  \"primitive\": \"channel\",");
    println!("  \"lang\": \"rust\",");
    println!("  \"mode\": \"{}\",", mode);
    println!("  \"duration_ms\": {},", DURATION_MS);
    println!("  \"total_ops\": {},", total_ops);
    println!("  \"duration_sec\": {:.6},", sec);
    println!("  \"ops_per_sec\": {:.0},", ops);
    println!("  \"ns_per_op\": {:.2}", nspo);
    println!("}}");
}

fn create_runtime(workers: usize) -> Runtime {
    Builder::new_multi_thread()
        .worker_threads(workers)
        .enable_time()
        .build()
        .unwrap()
}

async fn send_async(
    tx: tokio_mpsc::UnboundedSender<usize>,
    running: Arc<AtomicBool>,
    payload: usize,
) {
    let mut spins = 0_u32;
    while running.load(Ordering::Relaxed) {
        let _ = tx.send(payload);
        spins = spins.wrapping_add(1);
        if spins & 0xff == 0 {
            tokio::task::yield_now().await;
        }
    }
}

async fn recv_async(
    mut rx: tokio_mpsc::UnboundedReceiver<usize>,
    running: Arc<AtomicBool>,
    counter: Arc<AtomicU64>,
) {
    while let Some(_) = rx.recv().await {
        if !running.load(Ordering::Relaxed) {
            break;
        }
        counter.fetch_add(1, Ordering::Relaxed);
    }
}

fn send_thrd(tx: tokio_mpsc::UnboundedSender<usize>, running: Arc<AtomicBool>) {
    while running.load(Ordering::Relaxed) {
        let _ = tx.send(1);
    }
}

fn recv_thrd(rx: mpsc::Receiver<usize>, running: Arc<AtomicBool>, counter: Arc<AtomicU64>) {
    while let Ok(_) = rx.recv() {
        if !running.load(Ordering::Relaxed) {
            break;
        }
        counter.fetch_add(1, Ordering::Relaxed);
    }
}

async fn run_cc_async() {
    let (tx, rx) = tokio_mpsc::unbounded_channel();
    let running = Arc::new(AtomicBool::new(true));
    let counter = Arc::new(AtomicU64::new(0));

    let sender = tokio::spawn(send_async(tx.clone(), running.clone(), 1));
    let receiver = tokio::spawn(recv_async(rx, running.clone(), counter.clone()));

    let start = Instant::now();
    tokio::time::sleep(Duration::from_millis(DURATION_MS)).await;
    let elapsed = start.elapsed();
    running.store(false, Ordering::Relaxed);
    let total = counter.load(Ordering::Relaxed);
    let _ = sender.await;
    drop(tx);
    let _ = receiver.await;

    print_result("cc", total, elapsed);
}

fn run_tt() {
    let (tx, rx) = mpsc::channel();
    let running = Arc::new(AtomicBool::new(true));
    let counter = Arc::new(AtomicU64::new(0));

    let sender = {
        let running = running.clone();
        thread::spawn(move || {
            while running.load(Ordering::Relaxed) {
                let _ = tx.send(1);
            }
        })
    };
    let receiver = {
        let running = running.clone();
        let counter = counter.clone();
        thread::spawn(move || recv_thrd(rx, running, counter))
    };

    let start = Instant::now();
    thread::sleep(Duration::from_millis(DURATION_MS));
    let elapsed = start.elapsed();
    running.store(false, Ordering::Relaxed);
    let total = counter.load(Ordering::Relaxed);
    let _ = sender.join();
    let _ = receiver.join();

    print_result("tt", total, elapsed);
}

fn run_ct(rt: &Runtime) {
    let (tx, rx) = mpsc::channel();
    let running = Arc::new(AtomicBool::new(true));
    let counter = Arc::new(AtomicU64::new(0));

    let sender = {
        let running = running.clone();
        rt.spawn(async move {
            let mut spins = 0_u32;
            while running.load(Ordering::Relaxed) {
                let _ = tx.send(1);
                spins = spins.wrapping_add(1);
                if spins & 0xff == 0 {
                    tokio::task::yield_now().await;
                }
            }
        })
    };
    let receiver = {
        let running = running.clone();
        let counter = counter.clone();
        thread::spawn(move || recv_thrd(rx, running, counter))
    };

    let start = Instant::now();
    rt.block_on(async { tokio::time::sleep(Duration::from_millis(DURATION_MS)).await });
    let elapsed = start.elapsed();
    running.store(false, Ordering::Relaxed);
    let total = counter.load(Ordering::Relaxed);
    let _ = rt.block_on(sender);
    let _ = receiver.join();

    print_result("ct", total, elapsed);
}

fn run_tc(rt: &Runtime) {
    let (tx, rx) = tokio_mpsc::unbounded_channel();
    let running = Arc::new(AtomicBool::new(true));
    let counter = Arc::new(AtomicU64::new(0));

    let sender = {
        let running = running.clone();
        let tx = tx.clone();
        thread::spawn(move || send_thrd(tx, running))
    };
    let receiver = rt.spawn(recv_async(rx, running.clone(), counter.clone()));

    let start = Instant::now();
    rt.block_on(async { tokio::time::sleep(Duration::from_millis(DURATION_MS)).await });
    let elapsed = start.elapsed();
    running.store(false, Ordering::Relaxed);
    let total = counter.load(Ordering::Relaxed);
    let _ = sender.join();
    drop(tx);
    let _ = rt.block_on(receiver);

    print_result("tc", total, elapsed);
}

fn main() {
    let workers = default_workers();
    let rt = create_runtime(workers);

    rt.block_on(run_cc_async());
    run_tt();
    run_ct(&rt);
    run_tc(&rt);
}
