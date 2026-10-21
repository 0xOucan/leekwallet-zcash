# Host crates

The Rust code that runs on the companion side: a phone or a computer. It
syncs, builds PCZTs (ZIP 374), proves and broadcasts; the board only
checks and signs. One Cargo workspace, with librustzcash pinned to a git
commit (Ironwood, NU6.3, is newer than any release).

All four are ours (Apache-2.0). They are built on librustzcash, the
official Zcash Rust libraries; they are not official Zcash tools.

| Crate | What it is | Used by | README |
|---|---|---|---|
| `zecz-core` (lib `zecz`) | The wallet engine: profiles per network (a server of the other chain is refused), watch-only board accounts, lightwalletd sync with transparent spend detection, balances, rotating addresses, history, `propose_send` / `propose_shield` (full or compact signer view), applying a `.zsig`, proving, broadcast; `.zkv` seed cards; a DEV-only soft signer | `zecz-cli`, the desktop app, the Zcash LeekWallet app, tests | [zecz-core](zecz-core/README.md) |
| `zecz-cli` (bin `zecz`) | The command line over `zecz-core`: pair, import, sync, send, sign on the board, finish; `.zkv` cards | you, scripts | [zecz-cli](zecz-cli/README.md) |
| `leek-device` | The board's client: v2 pairing with a six-digit passkey, encrypted session, the `z*` calls over any `FramePort` (USB serial, Bluetooth), the 0x0528 network refusal worded for the user, one retry on 0x0300 right after connecting, and the `.zsig` format | `zecz-core`, `leek-android`, the desktop app | [leek-device](leek-device/README.md) |
| `leek-android` | JNI library for the Android app: the board's client over a Kotlin-owned USB or Bluetooth link, the compact signer view, applying a `.zsig` to the full PCZT | [leekwallet-zcash-android](https://github.com/0xOucan/leekwallet-zcash-android) | [leek-android](leek-android/README.md) |

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
`tools/leekwallet/build-native.sh` ([how](leek-android/README.md#build)).

## `zecz` in five commands

```sh
Z="target/release/zecz --network test --data-dir ~/zecz-device"   # or --network main
$Z device-init --port /dev/ttyACM0            # pair, import the board's account
$Z sync && $Z balance
$Z send --to <address> --amount 0.001 --zec --memo "hi" --compact --out tx.pczt
$Z sign-device --port /dev/ttyACM0 --in tx.pczt --out tx.zsig     # review and ACCEPT on the board
$Z finish --pczt tx.pczt --sigs tx.zsig       # apply, prove, broadcast
```

`zecz history` lists the account's transactions (a send to your own
address reads "Sent to yourself" with its fee as the amount);
`zecz shield --compact --out s.pczt` moves transparent coins into the
shielded pool; `zecz device-show-address` verifies the address on the
board; `zecz --help` lists the rest; every command is in
[zecz-cli/README.md](zecz-cli/README.md). Mainnet checklist:
[docs/zcash/MAINNET.md](../docs/zcash/MAINNET.md).
