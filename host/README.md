# Host crates

Website: [zcash.leekwallet.xyz](https://zcash.leekwallet.xyz) · Demo: [2-minute video](https://www.youtube.com/watch?v=8F6_8AhsnAA) · X: [@gmLeekWallet](https://x.com/gmLeekWallet) · [@0xOucan](https://x.com/0xOucan)

The Rust code that runs on the companion side: a phone or a computer. It
syncs, builds PCZTs (ZIP 374), proves and broadcasts; the board only
checks and signs. One Cargo workspace, with the librustzcash crates pinned
exactly to the releases that set the NU7 mainnet height (`zcash_primitives
0.31.0` and siblings, MSRV 1.89).

All seven are ours (Apache-2.0). They are built on librustzcash, the
official Zcash Rust libraries; they are not official Zcash tools.

| Crate | What it is | Used by | README |
|---|---|---|---|
| `zecz-core` (lib `zecz`) | The wallet engine: profiles per network (a server of the other chain is refused), watch-only board accounts, lightwalletd sync with transparent spend detection, balances, rotating addresses, history, `propose_send` / `propose_shield` (full or compact signer view), applying a `.zsig`, proving, broadcast; `.zkv` seed cards; a DEV-only soft signer | `zecz-cli`, the desktop app, the Zcash LeekWallet app, tests | [zecz-core](zecz-core/README.md) |
| `zecz-cli` (bin `zecz`) | The command line over `zecz-core`: pair, import, sync, send, sign on the board, finish; `.zkv` cards | you, scripts | [zecz-cli](zecz-cli/README.md) |
| `leek-device` | The board's client: v2 pairing with a six-digit passkey, encrypted session, the `z*` calls over any `FramePort` (USB serial, Bluetooth), the 0x0528 network refusal worded for the user, one retry on 0x0300 right after connecting, and the `.zsig` format | `zecz-core`, `leek-android`, the desktop app | [leek-device](leek-device/README.md) |
| `leek-zcash` | The LeekWallet SDK's Zcash kit: host-side preflight, filling missing derivations, the signer view, `sign_pczt` and `apply_zsig` (every signature verified, a signed PCZT out for the wallet's Keystone path), the SD-card calls (experimental), the account record, persisting a signed result with its txid, typed `LeekError` with stable codes. No transport of its own | wallet integrations: `zingo-cli` in our zingolib fork, and through `leek-zcash-ffi` | [leek-zcash](leek-zcash/README.md) |
| `leek-zcash-ffi` | UniFFI bindings of `leek-zcash` for Kotlin and Swift: byte API, errors by category with stable codes, a foreign `FrameTransport` so the app owns USB / BLE, progress and cancel. Generated bindings committed (WASM next) | our Zodl fork and our Zingo fork (Android); iOS wallets | [leek-zcash-ffi](leek-zcash-ffi/README.md) |
| `leek-ur` | BC-UR for the QR air gap: Bytewords, CRC-32, the fountain code, a multi-part encoder and a bounded decoder for `ur:leek-pczt` / `ur:leek-zsig`. The host twin of `firmware/src/ur*.c`, tested against the firmware's bc-ur vectors and the shared fixtures in `vectors/ur/`. `sha2` only | `zecz-cli`, the Zcash LeekWallet app | this table |
| `leek-android` | JNI library for the Android app: the board's client over a Kotlin-owned USB or Bluetooth link, the compact signer view, applying a `.zsig` to the full PCZT | [leekwallet-zcash-android](https://github.com/0xOucan/leekwallet-zcash-android) | [leek-android](leek-android/README.md) |

The Chrome extension cannot use `zecz-core` directly (tonic, files, tokio);
its engine in [`../chrome/engine`](../chrome/engine) reuses `zsig.rs` and the
pure modules and follows the same flow. Every wallet that reaches the board,
and over which links: [Wallets and links](../README.md#wallets-and-links).

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

### Testnet over QR

The same send with no cable, for a board kept off USB and Bluetooth. Slower
than a cable; never used for pairing (the import above is over USB).

```sh
cargo build --release -p zecz-cli --features qr-camera    # the webcam reader (Linux)
$Z sync
$Z send --to <testnet address> --amount 0.001 --zec --out tx.pczt --qr   # animated ur:leek-pczt here
# board: Scan, aim at the terminal, review, ACCEPT; it shows ur:leek-zsig
$Z qr-scan --pczt tx.pczt --out tx.zsig           # or --from-file parts.txt
$Z finish --pczt tx.pczt --sigs tx.zsig           # apply, prove, broadcast
```

A small terminal: `--qr-html tx-qr.html` writes an offline page that loops
the code in a browser. Defaults 50-byte fragments, 500 ms a frame (what the board read fastest)
(`--qr-fragment`, `--qr-interval`); `zecz qr-show --in tx.pczt --sweep`
steps through the speed presets for the camera bench
([QR-BENCH.md](../docs/zcash/QR-BENCH.md)); details in
[zecz-cli/README.md](zecz-cli/README.md#qr-instead-of-a-cable).

`zecz history` lists the account's transactions (a send to your own
address reads "Sent to yourself" with its fee as the amount);
`zecz shield --compact --out s.pczt` moves transparent coins into the
shielded pool; `zecz device-show-address` verifies the address on the
board; `zecz --help` lists the rest; every command is in
[zecz-cli/README.md](zecz-cli/README.md). Mainnet checklist:
[docs/zcash/MAINNET.md](../docs/zcash/MAINNET.md).
