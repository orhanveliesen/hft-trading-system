use hft_rust::{OrderBook, Side};
use std::time::Instant;

const WARMUP_OPS: usize = 1_000;
const BENCH_OPS: usize = 100_000;

fn main() {
    println!("=== Rust Order Book Benchmark ===\n");

    bench_add_order();
    bench_cancel_order();
    bench_execute_order();
    bench_best_bid_ask();
    bench_throughput();

    println!("=== Benchmark Complete ===");
}

fn bench_add_order() {
    let mut book = OrderBook::new();

    // Warmup
    for i in 0..WARMUP_OPS {
        let price = 90000 + (i as u32 % 20000);
        let side = if i % 2 == 0 { Side::Buy } else { Side::Sell };
        book.add_order(i as u64, side, price, 100);
    }

    // Reset
    book = OrderBook::new();

    let start = Instant::now();
    for i in 0..BENCH_OPS {
        let price = 90000 + (i as u32 % 20000);
        let side = if i % 2 == 0 { Side::Buy } else { Side::Sell };
        book.add_order(i as u64, side, price, 100);
    }
    let elapsed = start.elapsed();

    let ns_per_op = elapsed.as_nanos() as f64 / BENCH_OPS as f64;
    println!("Add Order:");
    println!("  Count: {} ops", BENCH_OPS);
    println!("  Mean:  {:.1} ns/op\n", ns_per_op);
}

fn bench_cancel_order() {
    let mut book = OrderBook::new();

    // Pre-fill
    for i in 0..BENCH_OPS {
        let price = 100000 + (i as u32 % 1000);
        let side = if i % 2 == 0 { Side::Buy } else { Side::Sell };
        book.add_order(i as u64, side, price, 100);
    }

    let start = Instant::now();
    for i in 0..BENCH_OPS {
        book.cancel_order(i as u64);
    }
    let elapsed = start.elapsed();

    let ns_per_op = elapsed.as_nanos() as f64 / BENCH_OPS as f64;
    println!("Cancel Order:");
    println!("  Count: {} ops", BENCH_OPS);
    println!("  Mean:  {:.1} ns/op\n", ns_per_op);
}

fn bench_execute_order() {
    let mut book = OrderBook::new();

    // Pre-fill
    for i in 0..BENCH_OPS {
        let price = 100000 + (i as u32 % 1000);
        let side = if i % 2 == 0 { Side::Buy } else { Side::Sell };
        book.add_order(i as u64, side, price, 1000);
    }

    let start = Instant::now();
    for i in 0..BENCH_OPS {
        book.execute_order(i as u64, 10);
    }
    let elapsed = start.elapsed();

    let ns_per_op = elapsed.as_nanos() as f64 / BENCH_OPS as f64;
    println!("Execute Order (Partial):");
    println!("  Count: {} ops", BENCH_OPS);
    println!("  Mean:  {:.1} ns/op\n", ns_per_op);
}

fn bench_best_bid_ask() {
    let mut book = OrderBook::new();

    // Pre-fill
    for i in 0..10_000 {
        let price = 100000 + (i as u32 % 100);
        let side = if i % 2 == 0 { Side::Buy } else { Side::Sell };
        book.add_order(i as u64, side, price, 100);
    }

    let start = Instant::now();
    let mut bid = 0u32;
    let mut ask = 0u32;
    for _ in 0..BENCH_OPS {
        bid = book.best_bid();
        ask = book.best_ask();
    }
    let elapsed = start.elapsed();

    // Prevent optimization
    std::hint::black_box((bid, ask));

    let ns_per_op = elapsed.as_nanos() as f64 / BENCH_OPS as f64;
    println!("Best Bid/Ask Query:");
    println!("  Count: {} ops", BENCH_OPS);
    println!("  Mean:  {:.1} ns/op\n", ns_per_op);
}

fn bench_throughput() {
    const OPS: usize = 1_000_000;
    let mut book = OrderBook::new();

    let start = Instant::now();
    for i in 0..OPS {
        let price = 100000 + (i as u32 % 1000);
        let side = if i % 2 == 0 { Side::Buy } else { Side::Sell };
        book.add_order((i % 100_000) as u64, side, price, 100);

        if i % 3 == 0 {
            book.cancel_order((i % 100_000) as u64);
        }
    }
    let elapsed = start.elapsed();

    let ops_per_sec = OPS as f64 / elapsed.as_secs_f64();
    let ns_per_op = elapsed.as_nanos() as f64 / OPS as f64;

    println!("Throughput (mixed workload):");
    println!("  {:.2} million ops/sec", ops_per_sec / 1_000_000.0);
    println!("  {:.2} ns/op average\n", ns_per_op);
}
