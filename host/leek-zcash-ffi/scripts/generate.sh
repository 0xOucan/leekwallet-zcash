#!/usr/bin/env bash
# Regenerates bindings/kotlin and bindings/swift from the compiled library
# (UniFFI library mode: the interface is read from the library's metadata,
# so the bindings always match the Rust). The generator is this crate's own
# `uniffi-bindgen` binary, built from the same pinned uniffi as the
# scaffolding. Run from anywhere; commit the result.
set -euo pipefail
crate="$(cd "$(dirname "$0")/.." && pwd)"
host="$(cd "$crate/.." && pwd)"
cd "$host"

cargo build -p leek-zcash-ffi --lib
case "$(uname -s)" in
    Darwin) lib="$host/target/debug/libleek_zcash_ffi.dylib" ;;
    *)      lib="$host/target/debug/libleek_zcash_ffi.so" ;;
esac

for lang in kotlin swift; do
    rm -rf "$crate/bindings/$lang"
    cargo run -q -p leek-zcash-ffi --features bindgen --bin uniffi-bindgen -- \
        generate --library "$lib" --language "$lang" --config "$crate/uniffi.toml" \
        --out-dir "$crate/bindings/$lang" --no-format
done
echo "bindings in $crate/bindings"
