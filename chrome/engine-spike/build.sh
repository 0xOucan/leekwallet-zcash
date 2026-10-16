#!/usr/bin/env bash
# Builds the spike for wasm32-unknown-unknown and runs wasm-bindgen.
#
#   ./build.sh prover          single-threaded prover        -> pkg-prover/
#   ./build.sh threads         rayon prover (atomics)         -> pkg-threads/
#   ./build.sh engine          prover + wallet engine crates  -> pkg-engine/
#
# C code (secp256k1-sys, and sqlite for the engine) needs a clang that targets
# wasm32. Any LLVM clang works; the Android NDK's does, so it is the default.
set -euo pipefail
cd "$(dirname "$0")"

NDK_BIN="${NDK_BIN:-$HOME/Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin}"
export CC_wasm32_unknown_unknown="${CC_wasm32_unknown_unknown:-$NDK_BIN/clang}"
export AR_wasm32_unknown_unknown="${AR_wasm32_unknown_unknown:-$NDK_BIN/llvm-ar}"
JOBS="${JOBS:-2}"

what="${1:-prover}"
case "$what" in
  prover)  features="";        extra=();;
  engine)  features="engine";  extra=();;
  sqlite)  features="sqlite";  extra=();;
  threads)
    features="threads"
    # Shared memory needs std rebuilt with atomics. Stable rustc accepts
    # -Z build-std under RUSTC_BOOTSTRAP=1 (needs `rustup component add rust-src`).
    export RUSTC_BOOTSTRAP=1
    export CARGO_TARGET_WASM32_UNKNOWN_UNKNOWN_RUSTFLAGS="-C target-feature=+atomics,+bulk-memory,+mutable-globals -C link-arg=--shared-memory -C link-arg=--max-memory=4294967296 -C link-arg=--import-memory -C link-arg=--export=__wasm_init_tls -C link-arg=--export=__tls_size -C link-arg=--export=__tls_align -C link-arg=--export=__tls_base"
    extra=(-Z build-std=panic_abort,std --target-dir target-threads)
    ;;
  *) echo "usage: $0 prover|threads|engine" >&2; exit 2;;
esac

cargo build -j"$JOBS" --release --target wasm32-unknown-unknown ${features:+--features "$features"} "${extra[@]}"

dir=target
[ "$what" = threads ] && dir=target-threads
wasm-bindgen --target web --out-dir "pkg-$what" "$dir/wasm32-unknown-unknown/release/engine_spike.wasm"
ls -l "pkg-$what"/*.wasm
