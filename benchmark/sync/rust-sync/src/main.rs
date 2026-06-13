// Rust sync-primitive microbenchmark.
//
// Mirrors the workloads in ../xylem-sync/main.c and ../go-sync so the JSON
// output lines up field-for-field.
//
//   sync-rust <primitive> [--mode coro|thread]
//             [--workers W] [--tasks T] [--iters N] [--permits K]
//
// Modes:
//   coro    -> Tokio tasks on a multi-threaded runtime, tokio::sync primitives
//   thread  -> std::thread + std::sync primitives (std::sync::mpsc for channel)
//   mixed   -> only the `channel` primitive: senders in one context feed a
//              receiver in the other over ONE shared MPSC. Works because a
//              channel's producer end is callable across the boundary
//              (tokio UnboundedSender::send and std mpsc Sender::send are both
//              plain sync calls), unlike the lock-style primitives (async
//              Mutex/Notify/Semaphore can't be used from a blocking thread, so
//              mutex/cond/sem/waitgroup stay unsupported in mixed and the
//              runner skips them).
//
// Workload model (identical across languages and modes):
//   mutex      : T workers each do N lock/inc/unlock          -> ops = T*N
//   cond       : 1 producer + 1 consumer, N hand-offs          -> ops = N
//   waitgroup  : N rounds over a pre-spawned pool of T workers  -> ops = T*N
//   sem        : T workers each do N acquire/release, K permits -> ops = T*N
//   channel    : T senders each send N msgs, 1 receiver         -> ops = T*N
//
// Tokio analogs (coro mode): cond -> Notify ping-pong; waitgroup -> a
// gate/fin handoff built from Semaphores (Tokio has no WaitGroup);
// channel -> mpsc::unbounded_channel.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc as stdmpsc;
use std::sync::{Arc, Condvar, Mutex as StdMutex};
use std::thread;
use std::time::{Duration, Instant};

use tokio::sync::{mpsc, Mutex, Notify, Semaphore};
use tokio::task::JoinSet;

struct Config {
    prim: String,
    mode: String,
    workers: usize,
    tasks: usize,
    iters: usize,
    permits: usize,
    chan_c2t: bool, // channel mixed: false = thread->coro (t2c), true = coro->thread (c2t)
}

fn main() {
    let mut cfg = Config {
        prim: String::new(),
        mode: "coro".to_string(),
        workers: 0,
        tasks: 8,
        iters: 100_000,
        permits: 4,
        chan_c2t: false,
    };

    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        usage(&args[0]);
        std::process::exit(2);
    }
    cfg.prim = args[1].clone();

    let mut i = 2;
    while i < args.len() {
        let has_val = i + 1 < args.len();
        match args[i].as_str() {
            "--mode" if has_val => {
                cfg.mode = args[i + 1].clone();
                i += 1;
            }
            "--workers" if has_val => {
                cfg.workers = args[i + 1].parse().unwrap_or(0);
                i += 1;
            }
            "--tasks" if has_val => {
                cfg.tasks = args[i + 1].parse().unwrap_or(8);
                i += 1;
            }
            "--iters" if has_val => {
                cfg.iters = args[i + 1].parse().unwrap_or(100_000);
                i += 1;
            }
            "--permits" if has_val => {
                cfg.permits = args[i + 1].parse().unwrap_or(4);
                i += 1;
            }
            "--chan-dir" if has_val => {
                match args[i + 1].as_str() {
                    "t2c" => cfg.chan_c2t = false,
                    "c2t" => cfg.chan_c2t = true,
                    other => {
                        eprintln!("rust: unknown --chan-dir {:?} (want t2c|c2t)", other);
                        std::process::exit(2);
                    }
                }
                i += 1;
            }
            _ => {
                usage(&args[0]);
                std::process::exit(2);
            }
        }
        i += 1;
    }

    if cfg.tasks < 1 {
        cfg.tasks = 1;
    }
    if cfg.iters < 1 {
        cfg.iters = 1;
    }
    if cfg.permits < 1 {
        cfg.permits = 1;
    }

    // handoff: a round-trip wake-latency probe. The mode selects the
    // context pair: coro = task<->task, thread = thread<->thread,
    // mixed = external thread <-> async task (the video-capture pattern).
    if cfg.prim == "handoff" {
        let (total_ops, elapsed) = match cfg.mode.as_str() {
            "coro" => handoff_cc(&cfg),
            "thread" => handoff_tt(&cfg),
            "mixed" => handoff_ct(&cfg),
            other => {
                eprintln!("rust: unknown mode {:?}", other);
                std::process::exit(2);
            }
        };
        print_result(&cfg, total_ops, elapsed);
        return;
    }

    let (total_ops, elapsed) = match cfg.mode.as_str() {
        "coro" => run_coro(&cfg),
        "thread" => run_thread(&cfg),
        "mixed" => {
            if cfg.prim == "channel" {
                mixed_channel(&cfg)
            } else {
                eprintln!("rust: mode \"mixed\" supported only for channel (async tasks and OS threads cannot share a lock-style primitive)");
                std::process::exit(3);
            }
        }
        other => {
            eprintln!("rust: unknown mode {:?}", other);
            std::process::exit(2);
        }
    };

    print_result(&cfg, total_ops, elapsed);
}

// ==========================================================================
// coro mode: Tokio
// ==========================================================================

fn run_coro(cfg: &Config) -> (u64, Duration) {
    let mut builder = tokio::runtime::Builder::new_multi_thread();
    if cfg.workers > 0 {
        builder.worker_threads(cfg.workers);
    }
    let rt = builder.build().expect("failed to build tokio runtime");
    rt.block_on(async {
        match cfg.prim.as_str() {
            "mutex" => coro_mutex(cfg).await,
            "cond" => coro_cond(cfg).await,
            "waitgroup" => coro_waitgroup(cfg).await,
            "sem" => coro_sem(cfg).await,
            "channel" => coro_channel(cfg).await,
            other => {
                eprintln!("rust: unknown primitive {:?}", other);
                std::process::exit(2);
            }
        }
    })
}

async fn coro_mutex(cfg: &Config) -> (u64, Duration) {
    let mu = Arc::new(Mutex::new(0u64));
    let mut set = JoinSet::new();
    let t0 = Instant::now();
    for _ in 0..cfg.tasks {
        let mu = mu.clone();
        let iters = cfg.iters;
        set.spawn(async move {
            for _ in 0..iters {
                let mut g = mu.lock().await;
                *g += 1;
            }
        });
    }
    while set.join_next().await.is_some() {}
    let elapsed = t0.elapsed();
    let total = cfg.tasks as u64 * cfg.iters as u64;
    let counter = *mu.lock().await;
    if counter != total {
        eprintln!("mutex: counter mismatch {} != {}", counter, total);
    }
    (total, elapsed)
}

async fn coro_cond(cfg: &Config) -> (u64, Duration) {
    let p2c = Arc::new(Notify::new());
    let c2p = Arc::new(Notify::new());
    let iters = cfg.iters;
    let t0 = Instant::now();
    let consumer = {
        let (p2c, c2p) = (p2c.clone(), c2p.clone());
        tokio::spawn(async move {
            for _ in 0..iters {
                p2c.notified().await;
                c2p.notify_one();
            }
        })
    };
    let producer = {
        let (p2c, c2p) = (p2c.clone(), c2p.clone());
        tokio::spawn(async move {
            for _ in 0..iters {
                p2c.notify_one();
                c2p.notified().await;
            }
        })
    };
    let _ = producer.await;
    let _ = consumer.await;
    (iters as u64, t0.elapsed())
}

async fn coro_waitgroup(cfg: &Config) -> (u64, Duration) {
    // Tokio has no WaitGroup, so the same gate/fin handoff is built from
    // Semaphores (the idiomatic Tokio building block). A fixed pool of T
    // tasks is spawned once, OUTSIDE the timed region, and loops over the
    // rounds. Each round uses a fresh pair of single-use semaphores:
    // gate[r] (main releases the pool) and fin[r] (the pool signals
    // completion). Task creation never enters the measurement.
    let rounds = cfg.iters;
    let t = cfg.tasks;

    let gate: Arc<Vec<Semaphore>> =
        Arc::new((0..rounds).map(|_| Semaphore::new(0)).collect());
    let fin: Arc<Vec<Semaphore>> =
        Arc::new((0..rounds).map(|_| Semaphore::new(0)).collect());

    let mut handles = Vec::with_capacity(t);
    for _ in 0..t {
        let gate = gate.clone();
        let fin = fin.clone();
        handles.push(tokio::spawn(async move {
            for r in 0..rounds {
                gate[r].acquire().await.unwrap().forget(); // wait for open
                fin[r].add_permits(1); // signal this worker is done
            }
        }));
    }

    let t0 = Instant::now();
    for r in 0..rounds {
        gate[r].add_permits(t); // release the pool
        fin[r].acquire_many(t as u32).await.unwrap().forget(); // join
    }
    let elapsed = t0.elapsed();

    for h in handles {
        let _ = h.await;
    }
    (rounds as u64 * cfg.tasks as u64, elapsed)
}

async fn coro_sem(cfg: &Config) -> (u64, Duration) {
    let sem = Arc::new(Semaphore::new(cfg.permits));
    let mut set = JoinSet::new();
    let t0 = Instant::now();
    for _ in 0..cfg.tasks {
        let sem = sem.clone();
        let iters = cfg.iters;
        set.spawn(async move {
            for _ in 0..iters {
                let permit = sem.acquire().await.unwrap();
                drop(permit);
            }
        });
    }
    while set.join_next().await.is_some() {}
    (cfg.tasks as u64 * cfg.iters as u64, t0.elapsed())
}

async fn coro_channel(cfg: &Config) -> (u64, Duration) {
    let (tx, mut rx) = mpsc::unbounded_channel::<()>();
    let total = cfg.tasks as u64 * cfg.iters as u64;
    let got = Arc::new(AtomicU64::new(0));
    let t0 = Instant::now();
    let recv_got = got.clone();
    let receiver = tokio::spawn(async move {
        let mut n: u64 = 0;
        while rx.recv().await.is_some() {
            n += 1;
        }
        recv_got.store(n, Ordering::Relaxed);
    });
    let mut set = JoinSet::new();
    for _ in 0..cfg.tasks {
        let tx = tx.clone();
        let iters = cfg.iters;
        set.spawn(async move {
            for _ in 0..iters {
                let _ = tx.send(());
            }
        });
    }
    drop(tx);
    while set.join_next().await.is_some() {}
    let _ = receiver.await;
    let elapsed = t0.elapsed();
    let n = got.load(Ordering::Relaxed);
    if n != total {
        eprintln!("channel: recv mismatch {} != {}", n, total);
    }
    (total, elapsed)
}

// ==========================================================================
// thread mode: std::thread + std::sync
// ==========================================================================

fn run_thread(cfg: &Config) -> (u64, Duration) {
    match cfg.prim.as_str() {
        "mutex" => thread_mutex(cfg),
        "cond" => thread_cond(cfg),
        "waitgroup" => thread_waitgroup(cfg),
        "sem" => thread_sem(cfg),
        "channel" => thread_channel(cfg),
        other => {
            eprintln!("rust: unknown primitive {:?}", other);
            std::process::exit(2);
        }
    }
}

fn thread_mutex(cfg: &Config) -> (u64, Duration) {
    let mu = Arc::new(StdMutex::new(0u64));
    let t0 = Instant::now();
    let mut hs = Vec::with_capacity(cfg.tasks);
    for _ in 0..cfg.tasks {
        let mu = mu.clone();
        let iters = cfg.iters;
        hs.push(thread::spawn(move || {
            for _ in 0..iters {
                let mut g = mu.lock().unwrap();
                *g += 1;
            }
        }));
    }
    for h in hs {
        h.join().unwrap();
    }
    let elapsed = t0.elapsed();
    let total = cfg.tasks as u64 * cfg.iters as u64;
    let counter = *mu.lock().unwrap();
    if counter != total {
        eprintln!("mutex: counter mismatch {} != {}", counter, total);
    }
    (total, elapsed)
}

fn thread_cond(cfg: &Config) -> (u64, Duration) {
    // turn: 0 = producer's turn, 1 = consumer's turn
    let pair = Arc::new((StdMutex::new(0i32), Condvar::new()));
    let iters = cfg.iters;
    let t0 = Instant::now();
    let consumer = {
        let pair = pair.clone();
        thread::spawn(move || {
            let (m, c) = &*pair;
            for _ in 0..iters {
                let mut turn = m.lock().unwrap();
                while *turn != 1 {
                    turn = c.wait(turn).unwrap();
                }
                *turn = 0;
                c.notify_one();
            }
        })
    };
    let producer = {
        let pair = pair.clone();
        thread::spawn(move || {
            let (m, c) = &*pair;
            for _ in 0..iters {
                let mut turn = m.lock().unwrap();
                while *turn != 0 {
                    turn = c.wait(turn).unwrap();
                }
                *turn = 1;
                c.notify_one();
            }
        })
    };
    producer.join().unwrap();
    consumer.join().unwrap();
    (iters as u64, t0.elapsed())
}

fn thread_waitgroup(cfg: &Config) -> (u64, Duration) {
    // Persistent OS-thread pool + gate/fin built from the std Sem
    // (Mutex+Condvar) below. Threads are spawned once, OUTSIDE the timed
    // region; the measurement captures only the per-round release/join.
    let rounds = cfg.iters;
    let t = cfg.tasks;

    let gate: Arc<Vec<Sem>> = Arc::new((0..rounds).map(|_| Sem::new(0)).collect());
    let fin: Arc<Vec<Sem>> = Arc::new((0..rounds).map(|_| Sem::new(0)).collect());

    let mut hs = Vec::with_capacity(t);
    for _ in 0..t {
        let gate = gate.clone();
        let fin = fin.clone();
        hs.push(thread::spawn(move || {
            for r in 0..rounds {
                gate[r].acquire(); // wait for round r to open
                fin[r].release(); // signal this worker is done
            }
        }));
    }

    let t0 = Instant::now();
    for r in 0..rounds {
        for _ in 0..t {
            gate[r].release(); // release the pool
        }
        for _ in 0..t {
            fin[r].acquire(); // join the pool
        }
    }
    let elapsed = t0.elapsed();

    for h in hs {
        h.join().unwrap();
    }
    (rounds as u64 * cfg.tasks as u64, elapsed)
}

// std has no counting semaphore; a Mutex<count> + Condvar is the textbook one.
struct Sem {
    m: StdMutex<usize>,
    c: Condvar,
}
impl Sem {
    fn new(n: usize) -> Self {
        Sem {
            m: StdMutex::new(n),
            c: Condvar::new(),
        }
    }
    fn acquire(&self) {
        let mut n = self.m.lock().unwrap();
        while *n == 0 {
            n = self.c.wait(n).unwrap();
        }
        *n -= 1;
    }
    fn release(&self) {
        let mut n = self.m.lock().unwrap();
        *n += 1;
        self.c.notify_one();
    }
}

fn thread_sem(cfg: &Config) -> (u64, Duration) {
    let sem = Arc::new(Sem::new(cfg.permits));
    let t0 = Instant::now();
    let mut hs = Vec::with_capacity(cfg.tasks);
    for _ in 0..cfg.tasks {
        let sem = sem.clone();
        let iters = cfg.iters;
        hs.push(thread::spawn(move || {
            for _ in 0..iters {
                sem.acquire();
                sem.release();
            }
        }));
    }
    for h in hs {
        h.join().unwrap();
    }
    (cfg.tasks as u64 * cfg.iters as u64, t0.elapsed())
}

fn thread_channel(cfg: &Config) -> (u64, Duration) {
    // std::sync::mpsc is an unbounded MPSC -- a direct match for xylem's.
    let (tx, rx) = stdmpsc::channel::<()>();
    let total = cfg.tasks as u64 * cfg.iters as u64;
    let t0 = Instant::now();
    let receiver = thread::spawn(move || {
        let mut got: u64 = 0;
        while rx.recv().is_ok() {
            got += 1;
        }
        got
    });
    let mut hs = Vec::with_capacity(cfg.tasks);
    for _ in 0..cfg.tasks {
        let tx = tx.clone();
        let iters = cfg.iters;
        hs.push(thread::spawn(move || {
            for _ in 0..iters {
                let _ = tx.send(());
            }
        }));
    }
    drop(tx); // last sender clone gone after threads finish -> receiver ends
    for h in hs {
        h.join().unwrap();
    }
    let got = receiver.join().unwrap();
    let elapsed = t0.elapsed();
    if got != total {
        eprintln!("channel: recv mismatch {} != {}", got, total);
    }
    (total, elapsed)
}

// ==========================================================================
// mixed mode (channel only): cross-context senders -> receiver
// ==========================================================================

// One shared unbounded MPSC bridges a coroutine and an OS thread, mirroring
// xylem's channel `mixed` cell. The producer end of a channel is callable from
// either world (tokio UnboundedSender::send and std mpsc Sender::send are both
// non-blocking sync calls), so a clean single-direction handoff is expressible
// even though Rust cannot share a lock-style primitive across the boundary.
//
//   t2c (default): T OS-thread senders -> one async receiver task
//                  (thread -> coro, the video-capture direction)
//   c2t          : T async-task senders -> one blocking receiver thread
//                  (coro -> thread)
//
// The channel is unbounded, so only the receiver ever blocks and every wake
// runs in the pinned direction -- the same single-direction probe xylem runs.
fn mixed_channel(cfg: &Config) -> (u64, Duration) {
    let total = cfg.tasks as u64 * cfg.iters as u64;
    let mut builder = tokio::runtime::Builder::new_multi_thread();
    if cfg.workers > 0 {
        builder.worker_threads(cfg.workers);
    }
    let rt = builder.build().expect("failed to build tokio runtime");

    if !cfg.chan_c2t {
        // t2c: OS-thread senders feed an async receiver task (thread -> coro).
        let (tx, mut rx) = mpsc::unbounded_channel::<()>();
        let got = Arc::new(AtomicU64::new(0));
        let recv_got = got.clone();
        let t0 = Instant::now();
        let receiver = rt.spawn(async move {
            let mut n: u64 = 0;
            while rx.recv().await.is_some() {
                n += 1;
            }
            recv_got.store(n, Ordering::Relaxed);
        });
        let mut hs = Vec::with_capacity(cfg.tasks);
        for _ in 0..cfg.tasks {
            let tx = tx.clone();
            let iters = cfg.iters;
            hs.push(thread::spawn(move || {
                for _ in 0..iters {
                    let _ = tx.send(()); // OS thread -> async task (sync send)
                }
            }));
        }
        drop(tx); // close once every thread sender is gone -> receiver ends
        for h in hs {
            h.join().unwrap();
        }
        let _ = rt.block_on(receiver);
        let elapsed = t0.elapsed();
        let n = got.load(Ordering::Relaxed);
        if n != total {
            eprintln!("channel: recv mismatch {} != {}", n, total);
        }
        (total, elapsed)
    } else {
        // c2t: async-task senders feed a blocking receiver thread (coro -> thread).
        let (tx, rx) = stdmpsc::channel::<()>();
        let t0 = Instant::now();
        let receiver = thread::spawn(move || {
            let mut got: u64 = 0;
            while rx.recv().is_ok() {
                got += 1;
            }
            got
        });
        rt.block_on(async {
            let mut set = JoinSet::new();
            for _ in 0..cfg.tasks {
                let tx = tx.clone();
                let iters = cfg.iters;
                set.spawn(async move {
                    for _ in 0..iters {
                        let _ = tx.send(()); // async task -> OS thread (sync send)
                    }
                });
            }
            drop(tx); // close once every task sender is gone -> receiver ends
            while set.join_next().await.is_some() {}
        });
        let got = receiver.join().unwrap();
        let elapsed = t0.elapsed();
        if got != total {
            eprintln!("channel: recv mismatch {} != {}", got, total);
        }
        (total, elapsed)
    }
}

// ==========================================================================
// output
// ==========================================================================

// External OS thread <-> async task ping-pong: the "video capture" pattern
// where a capture thread hands frames to an async task that does the network
// send. Measures the round-trip thread<->task wake latency. thread->task uses
// a Tokio unbounded channel (its send is callable from any thread, no async
// context needed); task->thread uses a std mpsc (blocks the OS thread).
fn handoff_ct(cfg: &Config) -> (u64, Duration) {
    let n = cfg.iters;
    let mut builder = tokio::runtime::Builder::new_multi_thread();
    if cfg.workers > 0 {
        builder.worker_threads(cfg.workers);
    }
    let rt = builder.build().expect("failed to build tokio runtime");

    let (t2a_tx, mut t2a_rx) = mpsc::unbounded_channel::<()>();
    let (a2t_tx, a2t_rx) = stdmpsc::channel::<()>();

    let handle = rt.spawn(async move {
        for _ in 0..n {
            if t2a_rx.recv().await.is_none() {
                break; // wait for the external thread (thread -> task wake)
            }
            let _ = a2t_tx.send(()); // wake the external thread
        }
    });

    // This function runs on `main` -- a plain OS thread, external to Tokio,
    // exactly like a video-capture thread feeding an async network sender.
    let t0 = Instant::now();
    for _ in 0..n {
        let _ = t2a_tx.send(()); // thread -> task wake (the costly direction)
        let _ = a2t_rx.recv(); // wait for the task's reply (task -> thread)
    }
    let elapsed = t0.elapsed();

    let _ = rt.block_on(handle);
    (n as u64, elapsed)
}

// Two async tasks ping-pong over a pair of Tokio channels (task<->task wake).
fn handoff_cc(cfg: &Config) -> (u64, Duration) {
    let n = cfg.iters;
    let mut builder = tokio::runtime::Builder::new_multi_thread();
    if cfg.workers > 0 {
        builder.worker_threads(cfg.workers);
    }
    let rt = builder.build().expect("failed to build tokio runtime");
    rt.block_on(async move {
        let (ab_tx, mut ab_rx) = mpsc::unbounded_channel::<()>();
        let (ba_tx, mut ba_rx) = mpsc::unbounded_channel::<()>();
        let b = tokio::spawn(async move {
            for _ in 0..n {
                if ab_rx.recv().await.is_none() {
                    break;
                }
                let _ = ba_tx.send(());
            }
        });
        let t0 = Instant::now();
        for _ in 0..n {
            let _ = ab_tx.send(());
            if ba_rx.recv().await.is_none() {
                break;
            }
        }
        let elapsed = t0.elapsed();
        let _ = b.await;
        (n as u64, elapsed)
    })
}

// Two OS threads ping-pong over a pair of std mpsc channels (thread<->thread).
fn handoff_tt(cfg: &Config) -> (u64, Duration) {
    let n = cfg.iters;
    let (ab_tx, ab_rx) = stdmpsc::channel::<()>();
    let (ba_tx, ba_rx) = stdmpsc::channel::<()>();
    let b = thread::spawn(move || {
        for _ in 0..n {
            if ab_rx.recv().is_err() {
                break;
            }
            let _ = ba_tx.send(());
        }
    });
    let t0 = Instant::now();
    for _ in 0..n {
        let _ = ab_tx.send(());
        if ba_rx.recv().is_err() {
            break;
        }
    }
    let elapsed = t0.elapsed();
    let _ = b.join();
    (n as u64, elapsed)
}

fn print_result(cfg: &Config, total_ops: u64, elapsed: Duration) {
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
    println!("  \"primitive\": \"{}\",", cfg.prim);
    println!("  \"lang\": \"rust\",");
    println!("  \"mode\": \"{}\",", cfg.mode);
    println!("  \"workers\": {},", cfg.workers);
    println!("  \"tasks\": {},", cfg.tasks);
    println!("  \"iters\": {},", cfg.iters);
    if cfg.prim == "sem" {
        println!("  \"permits\": {},", cfg.permits);
    }
    println!("  \"total_ops\": {},", total_ops);
    println!("  \"duration_sec\": {:.6},", sec);
    println!("  \"ops_per_sec\": {:.0},", ops);
    println!("  \"ns_per_op\": {:.2}", nspo);
    println!("}}");
}

fn usage(prog: &str) {
    eprintln!(
        "usage: {} <mutex|cond|waitgroup|sem|channel|handoff> \
         [--mode coro|thread|mixed] [--workers W] [--tasks T] [--iters N] \
         [--permits K] [--chan-dir t2c|c2t]",
        prog
    );
}
