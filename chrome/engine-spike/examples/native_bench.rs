//! Native reference for the WASM numbers:
//!   cargo run --release --example native_bench [--features multicore]
use std::time::Instant;

fn main() {
    let t = Instant::now();
    let pk = engine_spike::prove::build_pk();
    println!("proving key build: {:.2} s", t.elapsed().as_secs_f64());
    for outputs in [1usize, 1, 3] {
        let t = Instant::now();
        let (actions, proof) = engine_spike::prove::prove(&pk, outputs);
        println!(
            "prove {actions} actions: {:.2} s ({} B proof)",
            t.elapsed().as_secs_f64(),
            proof.len()
        );
    }
}
