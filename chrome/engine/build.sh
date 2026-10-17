#!/usr/bin/env bash
# Builds the engine for wasm32-unknown-unknown with threads and runs
# wasm-bindgen into pkg/ (what chrome/extension packages).
#
#   ./build.sh            release build -> pkg/
#   JOBS=4 ./build.sh     more cargo jobs (default 2)
#
# Needs: rustup target wasm32-unknown-unknown, component rust-src (threads
# rebuild std), wasm-bindgen-cli matching Cargo.lock (0.2.129), and a clang
# that targets wasm32 for the C code (secp256k1-sys, SQLite). Any LLVM clang
# works; the Android NDK's is the default. See chrome/README.md.
set -euo pipefail
cd "$(dirname "$0")"

if [ -z "${CC_wasm32_unknown_unknown:-}" ]; then
  NDK_BIN="${NDK_BIN:-$HOME/Android/Sdk/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin}"
  if [ -x "$NDK_BIN/clang" ]; then
    export CC_wasm32_unknown_unknown="$NDK_BIN/clang"
    export AR_wasm32_unknown_unknown="$NDK_BIN/llvm-ar"
  elif command -v clang >/dev/null; then
    export CC_wasm32_unknown_unknown=clang
    export AR_wasm32_unknown_unknown="$(command -v llvm-ar || ls /usr/bin/llvm-ar-* 2>/dev/null | head -1)"
  else
    echo "no clang for wasm32: set NDK_BIN or CC_wasm32_unknown_unknown" >&2
    exit 1
  fi
fi
JOBS="${JOBS:-2}"

# Shared memory needs std rebuilt with atomics. Stable rustc accepts
# -Z build-std under RUSTC_BOOTSTRAP=1 (rustup component add rust-src).
export RUSTC_BOOTSTRAP=1
export CARGO_TARGET_WASM32_UNKNOWN_UNKNOWN_RUSTFLAGS="-C target-feature=+atomics,+bulk-memory,+mutable-globals -C link-arg=--shared-memory -C link-arg=--max-memory=4294967296 -C link-arg=--import-memory -C link-arg=--export=__wasm_init_tls -C link-arg=--export=__tls_size -C link-arg=--export=__tls_align -C link-arg=--export=__tls_base"
# C code linked into shared memory must be built for it too (sqlite3.o).
export CFLAGS_wasm32_unknown_unknown="${CFLAGS_wasm32_unknown_unknown:-} -matomics -mbulk-memory"

cargo build -j"$JOBS" --locked --release --target wasm32-unknown-unknown --features threads \
  -Z build-std=panic_abort,std

rm -rf pkg
wasm-bindgen --target web --out-dir pkg target/wasm32-unknown-unknown/release/leek_chrome_engine.wasm

# wasm-bindgen-rayon spawns each worker from a blob: URL. An MV3 extension
# page's CSP (script-src 'self' 'wasm-unsafe-eval') refuses that, and
# initThreadPool then waits forever (WebZjs issue #141). Spawn the worker
# from the packaged file instead, which is 'self'.
helper=$(ls pkg/snippets/wasm-bindgen-rayon-*/src/workerHelpers.no-bundler.js)
python3 - "$helper" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = """      let scriptBlob = await fetch(import.meta.url).then(r => r.blob());
      let url = URL.createObjectURL(scriptBlob);
      const worker = new Worker(url, {"""
new = """      // leekwallet: same-origin file, not a blob: URL (MV3 CSP).
      const url = import.meta.url;
      const worker = new Worker(url, {"""
assert old in s, "wasm-bindgen-rayon helper changed; update build.sh"
s = s.replace(old, new).replace("      URL.revokeObjectURL(url);\n", "")
open(p, "w").write(s)
PY

# wasm-opt, when installed, shrinks the binary (not required).
if command -v wasm-opt >/dev/null && [ -z "${NO_WASM_OPT:-}" ]; then
  wasm-opt -O2 --enable-threads --enable-bulk-memory --enable-mutable-globals --enable-sign-ext \
    --enable-nontrapping-float-to-int pkg/leek_chrome_engine_bg.wasm -o pkg/leek_chrome_engine_bg.wasm
fi
ls -l pkg/*.wasm
