# Host crates

The Rust code that runs on the companion side: a phone or a computer. It
syncs, builds PCZTs (ZIP 374), proves and broadcasts; the board only
checks and signs. One Cargo workspace, with librustzcash pinned to a git
commit (Ironwood, NU6.3, is newer than any release).

| Crate | What it is | Used by |
|---|---|---|
| [`zecz-core`](zecz-core) (lib `zecz`) | The wallet engine: profiles per network, watch-only and device accounts, lightwalletd sync (refuses a server of the other chain), balances, rotating addresses, history, `propose_send` / `propose_shield` (full or compact signer view), applying a `.zsig`, proving, extraction, broadcast; `.zkv` SD seed cards; a DEV-only soft signer | `zecz-cli`, the desktop app, tests |
| [`zecz-cli`](zecz-cli) (bin `zecz`) | The command line over `zecz-core` | you, scripts |
| [`leek-device`](leek-device) | The board's client: v2 pairing (X25519, passkey), encrypted session, the `z*` calls (`zGetAccount`, `zShowAddress`, `zSign*` …) over any `FramePort` (USB serial, Bluetooth), and the `.zsig` format (v1 shielded, v2 with transparent entries) | `zecz-core`, `leek-android`, desktop |
| [`leek-android`](leek-android) | JNI library for the Android app: the device client over a Kotlin-owned link, the compact signer view, applying signatures to the full PCZT | [leekwallet-zcash-android](https://github.com/0xOucan/leekwallet-zcash-android) |

The Chrome extension cannot use `zecz-core` directly (tonic, files, tokio);
its engine in [`../chrome/engine`](../chrome/engine) reuses `zsig.rs` and the
pure modules and follows the same flow.

## Build and test

```sh
cargo build --release -p zecz-cli      # target/release/zecz
cargo test                             # offline tests; zdev tests build the simulated board
cargo test -- --ignored                # also the testnet / mainnet network tests
```

`tests/device_zdev.rs` drives `zdev` (`firmware/sim`, the firmware's real
protocol and signer code on a PTY), so the device tests need a C compiler
and make. The Android library is built from `../android` with
`tools/leekwallet/build-native.sh`.

## `zecz` in five commands

```sh
Z="target/release/zecz --network test --data-dir ~/zecz-device"   # or --network main
$Z device-init --port /dev/ttyACM0            # pair, import the board's account
$Z sync && $Z balance
$Z send --to <address> --amount 0.001 --zec --memo "hi" --compact --out tx.pczt
$Z sign-device --port /dev/ttyACM0 --in tx.pczt --out tx.zsig     # review and ACCEPT on the board
$Z finish --pczt tx.pczt --sigs tx.zsig       # apply, prove, broadcast
```

`zecz shield --compact --out s.pczt` moves transparent coins into the
shielded pool; `zecz device-show-address` verifies the address on the
board; `zecz --help` lists the rest. Mainnet checklist:
[docs/zcash/MAINNET.md](../docs/zcash/MAINNET.md).
