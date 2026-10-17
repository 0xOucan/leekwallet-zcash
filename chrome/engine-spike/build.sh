#!/usr/bin/env bash
# Builds the spike for wasm32-unknown-unknown and runs wasm-bindgen.
#
#   ./build.sh prover          single-threaded prover        -> pkg-prover/
#   ./build.sh threads         rayon prover (atomics)         -> pkg-threads/
#   ./build.sh engine          prover + wallet engine crates  -> pkg-engine/
#   ./build.sh threads-engine  both, with rayon               -> pkg-threads-engine/
#   ./build.sh sqlite          engine + zcash_client_sqlite (fails: see README)
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
  threads|threads-engine)
    features="threads"
    [ "$what" = threads-engine ] && features="threads,engine"
    # Shared memory needs std rebuilt with atomics. Stable rustc accepts
    # -Z build-std under RUSTC_BOOTSTRAP=1 (needs `rustup component add rust-src`).
    export RUSTC_BOOTSTRAP=1
    export CARGO_TARGET_WASM32_UNKNOWN_UNKNOWN_RUSTFLAGS="-C target-feature=+atomics,+bulk-memory,+mutable-globals -C link-arg=--shared-memory -C link-arg=--max-memory=4294967296 -C link-arg=--import-memory -C link-arg=--export=__wasm_init_tls -C link-arg=--export=__tls_size -C link-arg=--export=__tls_align -C link-arg=--export=__tls_base"
    extra=(-Z build-std=panic_abort,std --target-dir target-threads)
    [ "$what" = threads-engine ] && extra=(-Z build-std=panic_abort,std --target-dir target-threads-engine)
    ;;
  *) echo "usage: $0 prover|engine|threads|threads-engine|sqlite" >&2; exit 2;;
esac

cargo build -j"$JOBS" --release --target wasm32-unknown-unknown ${features:+--features "$features"} "${extra[@]}"

dir=target
case "$what" in threads) dir=target-threads;; threads-engine) dir=target-threads-engine;; esac
wasm-bindgen --target web --out-dir "pkg-$what" "$dir/wasm32-unknown-unknown/release/engine_spike.wasm"

if [ "$what" = threads ] || [ "$what" = threads-engine ]; then
  # wasm-bindgen-rayon spawns each worker from a blob: URL. An MV3 extension
  # page's CSP (script-src 'self' 'wasm-unsafe-eval'; blob: is not allowed)
  # refuses that, and initThreadPool then waits forever (WebZjs issue #141).
  # Spawn the worker from the packaged file instead, which is 'self'.
  helper=$(ls "pkg-$what"/snippets/wasm-bindgen-rayon-*/src/workerHelpers.no-bundler.js)
  python3 - "$helper" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = """      let scriptBlob = await fetch(import.meta.url).then(r => r.blob());
      let url = URL.createObjectURL(scriptBlob);
      const worker = new Worker(url, {"""
new = """      // engine-spike: same-origin file, not a blob: URL (MV3 CSP).
      const url = import.meta.url;
      const worker = new Worker(url, {"""
assert old in s, "wasm-bindgen-rayon helper changed; update build.sh"
s = s.replace(old, new).replace("      URL.revokeObjectURL(url);\n", "")
open(p, "w").write(s)
PY
fi
ls -l "pkg-$what"/*.wasm
