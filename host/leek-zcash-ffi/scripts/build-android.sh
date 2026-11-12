#!/usr/bin/env bash
# Builds libleek_zcash_ffi.so for Android, 64-bit only (arm64-v8a, x86_64),
# and lays it out with the Kotlin bindings and the consumer R8 rules:
#
#   <out>/jniLibs/arm64-v8a/libleek_zcash_ffi.so
#   <out>/jniLibs/x86_64/libleek_zcash_ffi.so
#   <out>/kotlin/io/leekwallet/zcash/sdk/leek_zcash_ffi.kt
#   <out>/consumer-rules.pro
#
# <out> is `--out <dir>` (or `--out=<dir>`), else $LEEK_ANDROID_OUT, else a
# first argument that is not an option (the older form), else
# host/target/android (ignored by git; the Gradle module in android/ reads it
# from there). An integrator points it at their own module, for example
# `--out ../my-app/leek-sdk`, so nothing is written into host/target/android.
#
# Release profile, symbols stripped with llvm-strip after the bindings check
# (the exported uniffi_* functions stay in .dynsym; UniFFI reads its metadata
# from .symtab, so the check runs on the unstripped build), 16 KB ELF page alignment for Android 15+, local paths remapped so
# others can rebuild the same bytes. Plain cargo with the NDK's clang as the
# linker, the same way as the fork's tools/leekwallet/build-native.sh.
#
# Refuses to finish if the committed bindings/kotlin does not match the
# library: the bindings are regenerated from the arm64 .so just built (UniFFI
# library mode) and compared byte for byte. Run scripts/generate.sh and commit
# when it fails.
#
# Env: ANDROID_NDK_HOME (default ~/Android/Sdk/ndk/27.2.12479018),
# ANDROID_API (default 27, the app's minSdk), ABIS (default "arm64-v8a x86_64").
# Other arguments go to cargo build (for example --offline). --locked is always
# passed.
set -euo pipefail
crate="$(cd "$(dirname "$0")/.." && pwd)"
host="$(cd "$crate/.." && pwd)"
out="${LEEK_ANDROID_OUT:-}"
if [[ $# -gt 0 && "$1" != -* ]]; then out="$1"; shift; fi
cargo_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) [[ $# -ge 2 && -n "$2" ]] || { echo "--out needs a directory" >&2; exit 2; }
               out="$2"; shift 2 ;;
        --out=*) out="${1#--out=}"; [[ -n "$out" ]] || { echo "--out needs a directory" >&2; exit 2; }
                 shift ;;
        *) cargo_args+=("$1"); shift ;;
    esac
done
out="${out:-$host/target/android}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

ndk="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/27.2.12479018}"
bin="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
api="${ANDROID_API:-27}"
abis="${ABIS:-arm64-v8a x86_64}"
[[ -x "$bin/clang" ]] || { echo "no NDK clang in $bin (set ANDROID_NDK_HOME)" >&2; exit 1; }

triple() {
    case "$1" in
        arm64-v8a) echo aarch64-linux-android ;;
        x86_64)    echo x86_64-linux-android ;;
        *) echo "unsupported ABI $1: 64-bit only (arm64-v8a, x86_64)" >&2; exit 1 ;;
    esac
}

remap="--remap-path-prefix=$host=/leek/host"
remap+=" --remap-path-prefix=${CARGO_HOME:-$HOME/.cargo}=/cargo"
remap+=" --remap-path-prefix=${RUSTUP_HOME:-$HOME/.rustup}=/rustup"

cd "$host"
for abi in $abis; do
    t="$(triple "$abi")"
    T="$(echo "$t" | tr 'a-z-' 'A-Z_')"
    u="$(echo "$t" | tr '-' '_')"
    # Per-target variables only, so host build scripts are not rebuilt with them.
    export "CARGO_TARGET_${T}_LINKER=$bin/$t$api-clang"
    export "CARGO_TARGET_${T}_RUSTFLAGS=$remap -C link-arg=-Wl,-z,max-page-size=16384"
    export "CC_$u=$bin/$t$api-clang"
    export "CXX_$u=$bin/$t$api-clang++"
    export "AR_$u=$bin/llvm-ar"
    export "RANLIB_$u=$bin/llvm-ranlib"
    cargo build --release --locked -p leek-zcash-ffi --lib --target "$t" ${cargo_args[@]+"${cargo_args[@]}"}
done
rm -rf "$out/jniLibs" "$out/kotlin"

# Bindings: regenerate from the Android library itself and compare.
first="$(triple "$(set -- $abis; echo "$1")")"
gen="$(mktemp -d)"
trap 'rm -rf "$gen"' EXIT
cargo run -q --locked -p leek-zcash-ffi --features bindgen --bin uniffi-bindgen -- \
    generate --library "$host/target/$first/release/libleek_zcash_ffi.so" --language kotlin \
    --config "$crate/uniffi.toml" --out-dir "$gen" --no-format
[[ -f "$gen/io/leekwallet/zcash/sdk/leek_zcash_ffi.kt" ]] \
    || { echo "uniffi-bindgen generated nothing from the Android library" >&2; exit 1; }
if ! diff -r "$gen" "$crate/bindings/kotlin" >/dev/null; then
    diff -r "$gen" "$crate/bindings/kotlin" | head -20 >&2 || true
    echo "bindings/kotlin does not match the library: run scripts/generate.sh and commit" >&2
    exit 1
fi
readelf="$bin/llvm-readelf"
for abi in $abis; do
    mkdir -p "$out/jniLibs/$abi"
    "$bin/llvm-strip" --strip-all -o "$out/jniLibs/$abi/libleek_zcash_ffi.so" \
        "$host/target/$(triple "$abi")/release/libleek_zcash_ffi.so"
done
cp -r "$crate/bindings/kotlin" "$out/kotlin"
cp "$crate/android/consumer-rules.pro" "$out/consumer-rules.pro"

# Only system libraries, and 16 KB LOAD alignment.
for abi in $abis; do
    so="$out/jniLibs/$abi/libleek_zcash_ffi.so"
    bad="$("$readelf" -d "$so" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' \
        | grep -v -x -E 'libc\.so|libm\.so|libdl\.so|liblog\.so' || true)"
    [[ -z "$bad" ]] || { echo "$abi: unexpected NEEDED: $bad" >&2; exit 1; }
    # Android 15+ needs every LOAD aligned to 16 KB or a multiple of it.
    if ! "$readelf" -lW "$so" | awk '$1 == "LOAD" { n++; a = strtonum($NF); if (a < 16384 || a % 16384) bad = 1 }
            END { exit (n == 0 || bad) }'; then
        echo "$abi: a LOAD segment is not 16 KB aligned" >&2; exit 1
    fi
    printf '%-10s %9d bytes  %s\n' "$abi" "$(stat -c %s "$so")" "$so"
done
echo "bindings and consumer-rules.pro in $out"
