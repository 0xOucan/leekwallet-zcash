#!/usr/bin/env bash
# Phase 0.4: build the spike, run it under Espressif QEMU, cross-check on host.
set -euo pipefail
cd "$(dirname "$0")"
CRATE=../../components/zsign-rust
B=.pio/build/esp32s3
. ~/export-esp.sh >/dev/null 2>&1 || true

pio run -e esp32s3 >/dev/null
python3 ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 merge_bin \
    -o $B/qemu-flash.bin --flash_mode dio --flash_freq 80m --flash_size 4MB \
    --fill-flash-size 4MB 0x0 $B/bootloader.bin 0x8000 $B/partitions.bin \
    0x10000 $B/firmware.bin >/dev/null

timeout 900 qemu-system-xtensa -nographic -machine esp32s3 \
    -drive file=$B/qemu-flash.bin,if=mtd,format=raw > $B/qemu.log 2>&1 &
QPID=$!
for _ in $(seq 900); do grep -q "ZSPIKE done" $B/qemu.log 2>/dev/null && break; sleep 1; done
kill $QPID 2>/dev/null || true
grep ZSPIKE $B/qemu.log | grep -v "=" || true

(cd $CRATE && cargo +stable build --release -q)
cc -O1 -Wall -I$CRATE/include host/host_check.c $CRATE/platform/zsign_platform_host.c \
    $CRATE/target/release/libzsign_rust.a -lpthread -ldl -lm -o $B/host_check
$B/host_check $B/qemu.log
