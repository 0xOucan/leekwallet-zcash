//! Native reference for the engine flow on the repo's NU6.3 Ironwood vector:
//!   cargo run --release --features engine --example native_flow [--features multicore]
use std::time::Instant;

const VECTOR: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/../../vectors/pczt/iw_send_shielded.pczt");
/// The public test mnemonic the vectors are generated with (vectors/pczt.json).
const MNEMONIC: &str = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon art";

fn main() {
    let start = Instant::now();
    let now = move || start.elapsed().as_secs_f64() * 1000.0;
    let t = Instant::now();
    let pk = engine_spike::prove::build_pk();
    println!("proving key: {:.2} s", t.elapsed().as_secs_f64());
    let t = Instant::now();
    let vk = engine_spike::prove::build_vk();
    println!("verifying key: {:.2} s", t.elapsed().as_secs_f64());
    let bytes = std::fs::read(VECTOR).unwrap();
    let r = engine_spike::engine::sign_prove_extract(&bytes, MNEMONIC, &pk, &vk, now).unwrap();
    println!("{r:?}");
}
