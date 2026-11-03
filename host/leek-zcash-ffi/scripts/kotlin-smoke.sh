#!/usr/bin/env bash
# JVM smoke test of the committed Kotlin bindings (tests/kotlin/Smoke.kt)
# against a freshly built libleek_zcash_ffi. UniFFI's checksums make a stale
# bindings file fail here, so run generate.sh after changing the API.
#
# Needs a JDK, the Kotlin compiler (embeddable) and JNA. By default they are
# taken from the Gradle cache (~/.gradle, filled by any Android build);
# override with KOTLIN_COMPILER_CP (classpath to run K2JVMCompiler),
# KOTLIN_STDLIB and JNA_JAR. Exits 0 with SKIPPED if they are not found.
set -euo pipefail
crate="$(cd "$(dirname "$0")/.." && pwd)"
host="$(cd "$crate/.." && pwd)"
gc="${GRADLE_USER_HOME:-$HOME/.gradle}/caches/modules-2/files-2.1"
jar() { find "$gc/$1/$2" -name "$2-$3.jar" 2>/dev/null | head -1; }
kv="${KOTLIN_VERSION:-2.0.21}"

stdlib="${KOTLIN_STDLIB:-$(jar org.jetbrains.kotlin kotlin-stdlib "$kv")}"
jna="${JNA_JAR:-$(find "$gc/net.java.dev.jna/jna" -name 'jna-5.*.jar' 2>/dev/null | sort -V | tail -1)}"
if [[ -z "${KOTLIN_COMPILER_CP:-}" ]]; then
    parts=(
        "$(jar org.jetbrains.kotlin kotlin-compiler-embeddable "$kv")" "$stdlib"
        "$(jar org.jetbrains.kotlin kotlin-script-runtime "$kv")" "$(jar org.jetbrains.kotlin kotlin-reflect "$kv")"
        "$(jar org.jetbrains.kotlin kotlin-daemon-embeddable "$kv")"
        "$(find "$gc/org.jetbrains.intellij.deps/trove4j" -name '*.jar' 2>/dev/null | head -1)"
        "$(find "$gc/org.jetbrains.kotlinx/kotlinx-coroutines-core-jvm" -name '*.jar' 2>/dev/null | head -1)"
        "$(jar org.jetbrains annotations 13.0)"
    )
    for p in "${parts[@]}"; do
        [[ -n "$p" ]] || { echo "SKIPPED: Kotlin compiler jars not in $gc (set KOTLIN_COMPILER_CP)"; exit 0; }
    done
    KOTLIN_COMPILER_CP="$(IFS=:; echo "${parts[*]}")"
fi
command -v java >/dev/null || { echo "SKIPPED: no JDK"; exit 0; }
[[ -n "$stdlib" && -n "$jna" ]] || { echo "SKIPPED: kotlin-stdlib or JNA not found (set KOTLIN_STDLIB, JNA_JAR)"; exit 0; }

# The bindings need JNA 5.12+ (com.sun.jna.internal.Cleaner); with an older
# one, compile the test-only shim in front of it.
shim=()
jv="$(basename "$jna" .jar)"; jv="${jv#jna-}"
if [[ "$(printf '%s\n' "$jv" 5.12.0 | sort -V | head -1)" != 5.12.0 ]]; then
    echo "JNA $jv is older than 5.12: using tests/kotlin/JnaCleanerShim.kt"
    shim=("$crate/tests/kotlin/JnaCleanerShim.kt")
fi

cd "$host"
cargo build -p leek-zcash-ffi --lib
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
java -cp "$KOTLIN_COMPILER_CP" org.jetbrains.kotlin.cli.jvm.K2JVMCompiler -no-stdlib -no-reflect \
    -cp "$stdlib:$jna" -d "$out" -nowarn \
    "$crate/bindings/kotlin/io/leekwallet/zcash/sdk/leek_zcash_ffi.kt" "$crate/tests/kotlin/Smoke.kt" ${shim[@]+"${shim[@]}"}
java -Djna.library.path="$host/target/debug" -cp "$out:$stdlib:$jna" SmokeKt "$host/../vectors"
