use std::sync::{Arc, Condvar, Mutex};
use std::thread;
use std::time::{Duration, Instant};

const DURATION_MS: u64 = 5000;

struct State {
    running: bool,
    turn: usize,
    counter: u64,
}

fn peer(pair: Arc<(Mutex<State>, Condvar)>, me: usize) {
    let other = 1 - me;

    loop {
        let (mutex, cond) = &*pair;
        let mut state = mutex.lock().unwrap();
        while state.turn != me && state.running {
            state = cond.wait(state).unwrap();
        }
        if !state.running {
            state.turn = other;
            cond.notify_one();
            break;
        }
        state.counter += 1;
        state.turn = other;
        cond.notify_one();
    }
}

fn print_result(total_ops: u64, elapsed: Duration) {
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
    println!("  \"primitive\": \"cond\",");
    println!("  \"lang\": \"rust\",");
    println!("  \"mode\": \"tt\",");
    println!("  \"duration_ms\": {},", DURATION_MS);
    println!("  \"total_ops\": {},", total_ops);
    println!("  \"duration_sec\": {:.6},", sec);
    println!("  \"ops_per_sec\": {:.0},", ops);
    println!("  \"ns_per_op\": {:.2}", nspo);
    println!("}}");
}

fn run_tt() {
    let pair = Arc::new((
        Mutex::new(State {
            running: true,
            turn: 0,
            counter: 0,
        }),
        Condvar::new(),
    ));

    let start = Instant::now();
    let peer0 = {
        let pair = pair.clone();
        thread::spawn(move || peer(pair, 0))
    };
    let peer1 = {
        let pair = pair.clone();
        thread::spawn(move || peer(pair, 1))
    };

    thread::sleep(Duration::from_millis(DURATION_MS));

    let (mutex, cond) = &*pair;
    {
        let mut state = mutex.lock().unwrap();
        state.running = false;
        cond.notify_all();
    }

    let _ = peer0.join();
    let _ = peer1.join();
    let elapsed = start.elapsed();
    let total_ops = mutex.lock().unwrap().counter;
    print_result(total_ops, elapsed);
}

fn main() {
    run_tt();
}
