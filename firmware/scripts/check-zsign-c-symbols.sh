#!/usr/bin/env bash
# The C signer must not share a single global with trezor-crypto, in either
# direction: it defines only zcv_* copies of the vendored modules, and it
# calls none of trezor-crypto's functions. A build that drops the prefix
# header still links, silently mixing two bignum implementations; this is
# the check that notices. Run after `pio run -e <env>`.
set -euo pipefail
ENV=${1:-esp32s3}
B=.pio/build/$ENV
NM=$(ls ~/.platformio/packages/toolchain-xtensa-esp-elf/bin/*-elf-nm 2>/dev/null | head -1)
ZC=$(find "$B" -name libzsign-c.a | head -1)
TZ=$(find "$B" -name libtrezor-crypto.a | head -1)
[ -n "$ZC" ] && [ -n "$TZ" ] || { echo "build $ENV first"; exit 2; }
tz=$(mktemp); trap 'rm -f "$tz"' EXIT
"$NM" -g --defined-only "$TZ" | awk 'NF==3{print $3}' | sort -u > "$tz"
shared=$( { "$NM" -g --defined-only "$ZC" | awk 'NF==3{print $3}'; "$NM" -u "$ZC" | awk '{print $2}'; } |
          sort -u | comm -12 - "$tz")
if [ -n "$shared" ]; then
    echo "zsign-c shares symbols with trezor-crypto:"; echo "$shared"; exit 1
fi
echo "zsign-c: $("$NM" -g --defined-only "$ZC" | grep -c ' zcv_') zcv_* symbols, none shared with trezor-crypto"
