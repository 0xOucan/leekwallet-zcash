# Toolchain (pinned)

| Tool | Version | Install |
|---|---|---|
| PlatformIO platform | espressif32@6.12.0 (ESP-IDF 5.5.0, GCC 14.2.0) | pinned in `platformio.ini` |
| Rust (host) | stable 1.97.1 | rustup |
| Rust (Xtensa) | esp 1.99.0.0 | `espup install --targets esp32s3 --toolchain-version 1.99.0.0` (espup 0.17.1) |
| Rust target | `xtensa-esp32s3-none-elf`, `-Zbuild-std=core,alloc` | — |
| QEMU | Espressif 9.2.2 (esp_develop_9.2.2_20260417) | release tarball → `~/.espressif/qemu`, linked into `~/.local/bin` |

Before any Xtensa cargo build: `. ~/export-esp.sh`.

## Known fixes
- `radium` 0.7 (via `bitvec`) assumes 64-bit atomics; Xtensa has 32-bit only.
  Vendored at `components/zsign-rust/vendor/radium` with an `xtensa-*` rule.
- Linking the `no_std` staticlib on the host needs a stub `rust_eh_personality`
  (`platform/zsign_platform_host.c`).
- PlatformIO's ESP-IDF integration ignores CMake custom targets, so cargo runs
  from `scripts/zsign_cargo.py` (a `pre:` extra script).
- C code in Rust dependencies (libsecp256k1 via secp256k1-sys) must be built
  with `xtensa-esp32s3-elf-gcc`. Left to itself, cc-rs picked the unified
  `xtensa-esp-elf` compiler, whose default core is **big-endian**, and
  produced big-endian objects. `components/zsign-rust/.cargo/config.toml`
  pins CC/AR for the target. Check with `readelf -h` (Data: little endian).
