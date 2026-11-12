#!/usr/bin/env bash
# Builds host/leek-ur for the extension's pages: wasm32-unknown-unknown, no
# threads, no C code, then wasm-bindgen into pkg/ (what chrome/extension
# packages as ur/). Seconds, not the engine's minutes.
#
#   ./build.sh            release build -> pkg/
#
# Needs: rustup target wasm32-unknown-unknown and wasm-bindgen-cli 0.2.129
# (the engine's, = Cargo.lock). See chrome/README.md.
set -euo pipefail
cd "$(dirname "$0")"

cargo build --locked --release --target wasm32-unknown-unknown

rm -rf pkg
wasm-bindgen --target web --out-dir pkg target/wasm32-unknown-unknown/release/leek_chrome_ur.wasm
ls -l pkg/*.wasm
