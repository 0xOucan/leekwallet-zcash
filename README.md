# LeekWallet Zcash

[![app](https://github.com/0xOucan/leekwallet-zcash/actions/workflows/app.yml/badge.svg)](https://github.com/0xOucan/leekwallet-zcash/actions/workflows/app.yml)

Website: [zcash.leekwallet.xyz](https://zcash.leekwallet.xyz) · Demo: [2-minute video](https://www.youtube.com/watch?v=8F6_8AhsnAA) · X: [@gmLeekWallet](https://x.com/gmLeekWallet) · [@0xOucan](https://x.com/0xOucan)

A DIY Zcash hardware wallet on an ESP32-S3 board that costs about as much as
lunch. It holds the seed, checks every transaction against its own keys, shows
you what you are signing, and signs it. Your phone or computer builds and proves
the transaction; the keys never leave the device.

Built on [LeekWallet](https://github.com/0xoucan/leekwallet), an open-source ESP32 hardware wallet
for Ethereum, turned into a Zcash-only one (firmware and every app here),
for Zcash's shielded pools: **Ironwood** (NU6.3), **Orchard**, and transparent
outputs.

> **ZECATHON provenance.** The starting point was my own earlier
> [LeekWallet](https://github.com/0xoucan/leekwallet) for Ethereum (this repo's
> history up to tag `pre-zecathon`, 2026-09-23): the board's screen, buttons,
> PIN, encrypted vault and USB/Bluetooth transport. Everything Zcash was written
> during the hackathon window, from 2026-10-14: the PCZT signer and address
> checks in the firmware, the network guard, the Rust libraries (zecz-core,
> leek-device, leek-android), the Zodl fork with hardware signing, the Zcash
> LeekWallet app (desktop and Android), the Chrome extension and the NEAR
> swaps. The EVM code was removed. `git diff pre-zecathon..main` shows the
> hackathon's work.

> **Status: experimental.** It has signed real testnet transactions over USB,
> Bluetooth, QR and SD card, and its first mainnet send and shield. It is not audited. Flash encryption and secure boot
> are deliberately not enabled (no eFuses burned), so anyone holding the board
> can read its flash. Use only small amounts on mainnet.

## How it works

```
 companion (phone / PC)                         LeekWallet board
 ──────────────────────                         ────────────────
 sync, propose, build PCZT ──── PCZT (ZIP 374) ───▶ parse, check spends are ours,
                               USB · BLE · QR · SD    recompute sighash, rebuild
                                                      notes, show outputs + memo
 apply signatures, prove,  ◀─── .zsig (~110 B) ──── user presses ACCEPT: RedPallas
 broadcast                                            spend-auth signatures only
```

- The device is a **PCZT signer**. Proving happens on the companion; the board
  never needs the proving keys.
- It signs the **compact signer view** (v2 PCZT, ~1.4 KB): the fields it can
  recompute (note commitments, value commitments, memo ciphertext) are left
  out and rebuilt on the device, so the memo it encrypts is the memo it showed.
- It returns a **`.zsig`**, signatures only, bound to the SHA-256 of the exact
  bytes it reviewed ([FORMATS.md](docs/zcash/FORMATS.md)).
- Pairing is an encrypted session (X25519, HKDF, ChaCha20-Poly1305) confirmed by
  a six-digit passkey on both screens ([PROTOCOL.md](firmware/docs/PROTOCOL.md)).

## Ecosystem

Every product here is Zcash only: no Ethereum or other EVM code in the
firmware or in any app. All of it is our work. The firmware and the Zcash
LeekWallet app started as forks of [LeekWallet](firmware/README.md); the
Android app is a fork of [Zodl](https://github.com/zodl-inc/zodl-android)
and lives in its own repository, under Zodl's MIT license. Everything in
this repository is Apache-2.0.

| Product | What it is | Platforms | Networks | Status | README |
|---|---|---|---|---|---|
| **Board firmware** (`firmware/`) | The signer. It holds the seed, checks each PCZT against its own keys, shows it, and signs. Forked from LeekWallet (an ESP32 Ethereum wallet) with its history, now Zcash only | ESP32-S3 board | Mainnet or Testnet, a setting on the board; it refuses requests for the other one | Done, tested on the board: shielded and transparent spends over USB, Bluetooth, QR and SD; compact PCZT; address check; first mainnet send and shield | [firmware](firmware/README.md) (upstream's README, with a Zcash note on top) |
| **Zcash LeekWallet** (`app/`) | Our app, id `io.leekwallet.zcash.companion`, built from LeekWallet's Companion with its EVM side removed, and since 2026-10-21 the one app: the desktop app merged into it. A full wallet: connects and sets up by itself, syncs, receives, sends and shields signed on the board; watch-only wallets from a viewing key; swaps from and into ZEC through NEAR Intents (1Click) | Linux, Windows, macOS, Android (Tauri 2) | Mainnet and Testnet in one app, their data apart; swaps on mainnet only | On the real board and an Android tablet over USB and Bluetooth: connects and sets up by itself, transparent and shielded sends, shield, rotating addresses verified on the board, passphrase wallet, QR scan for paying. The desktop app it absorbed did the first mainnet swap (ZEC → USDC on Base, the deposit signed on the board); swaps from the tablet next | [app](app/README.md) |
| **Chrome extension** (`chrome/`) | A Zcash wallet in the browser: librustzcash in WebAssembly, the wallet on OPFS, gRPC-web sync; the board over Web Serial and Web Bluetooth | Chromium browsers (tested in Brave) | Mainnet and Testnet | On the board: sends signed over USB on testnet and mainnet, Bluetooth pairing and address check. Software wallets next | [chrome](chrome/README.md) |
| **LeekWallet Zcash for Android** ([leekwallet-zcash-android](https://github.com/0xOucan/leekwallet-zcash-android), branch `leekwallet`) | Our fork of Zodl (MIT). Two apps that install side by side: **LeekWallet Zcash** (mainnet) and **LeekWallet Zcash (testnet)**. Everything Zodl does, plus an account whose keys stay on the board, signed over USB, Bluetooth or QR | Android | One app per network | Tested on testnet: import, send, shield, "Verify on device". Mainnet: connects, shows the balance and verifies addresses on the board; it refuses a board set to the other network | [in its repo](https://github.com/0xOucan/leekwallet-zcash-android#readme) |
| **`zecz` CLI** (`host/zecz-cli`) | Our command-line wallet: pair, import, sync, build a PCZT, sign on the board, prove, broadcast; `.zkv` SD seed cards | A computer with Rust (used on Linux) | Testnet (default) and Mainnet, one data directory per network | Full flow on testnet; mainnet sends and a shield signed on the board | [host/zecz-cli](host/zecz-cli/README.md) |
| **Libraries** (`host/`) | `zecz-core`, the wallet engine behind `zecz` and the Zcash LeekWallet app; `leek-device`, the board's client and the `.zsig` format; `leek-android`, the JNI library the Android fork loads | Rust | Both | In use by every companion above | [zecz-core](host/zecz-core/README.md), [leek-device](host/leek-device/README.md), [leek-android](host/leek-android/README.md) |
| **zdev** (`firmware/sim`) | A simulated board: the firmware's own protocol, signers and review code on a PTY, so companions can be tested without hardware. Development only | Linux | Both (`--network`) | Used by the host, app and Chrome tests | [firmware/sim](firmware/sim/README.md) |
| **Test vectors** (`vectors/`) | Keys, PCZTs (valid and refused), a `.zsig` and a `.zkv`, generated from librustzcash and shared by every implementation | | Both | | [vectors](vectors/README.md) |

**One app.** The Zcash LeekWallet app absorbed the desktop app (with its
NEAR swaps) on 2026-10-21 and moved to `app/`: a single app for desktop and
Android. On its first start it copies the old desktop app's wallets,
address books and swaps (never moving or deleting them).

**`zecz` is ours, not an official Zcash tool.** It is built on `zecz-core`,
our engine, which uses librustzcash (the official Zcash Rust libraries) for
keys, sync, PCZTs, proving and the wallet database. The official tools are
full nodes and their wallets: zcashd (retired), zebrad (a full node, with no
wallet) and Zallet (the full-node wallet that replaces zcashd's, in beta).
None of them knows the board. `zecz` is a light wallet: it syncs from a lightwalletd server, holds
only the board's viewing key, and hands each transaction to the board to
sign.

## Status

- **Mainnet: ready.** Mainnet vectors and tests, servers of the wrong chain
  refused, one birthday per network on the board, which refuses any request
  for the other network than its own setting, and a Testnet/Mainnet switch
  in the app with a real-funds confirmation. The board has signed
  mainnet sends and a shield, from `zecz` and from the Chrome extension.
  Testnet stays the default. Checklist: [MAINNET.md](docs/zcash/MAINNET.md).
- **NEAR Intents**, in the Zcash LeekWallet app on desktop and Android:
  swaps from and into ZEC through 1Click, the ZEC deposit signed on the
  board, status tracking and fresh shielded refund addresses (mainnet
  only).

## What works

| | |
|---|---|
| Keys | BIP39 seed + optional passphrase entered on the device, ZIP 32 accounts, unified addresses, seed fingerprint (ZFP) |
| Storage | PIN vault in flash, stateless (seed typed each boot), or an encrypted `.zkv` file on the microSD card |
| Signing | Ironwood and Orchard spends, and transparent P2PKH inputs (shielding), to shielded or transparent outputs, with memos. Two independent signers: C (on the device) and Rust (host and QEMU), cross-checked against each other |
| Links | USB serial and Bluetooth LE for everyday signing; animated QR (UR fountain codes, the board's camera) and the SD card when the board must stay off every link |
| Screen | 128×128 SH1107 OLED, EC11 rotary encoder + buttons; receive address as text and QR |
| Address check | The app sends the address it displays, even a rotated diversified one; the board proves it is its own (Orchard `pk_d = [ivk]·g_d`, index recovered by FF1 decryption, transparent receiver checked at that index) and only then shows it to confirm. Foreign or mixed addresses are refused |
| Companions | See [Ecosystem](#ecosystem) |

Testnet transactions signed on the board include
`1daaff6366fccbd6ea50f137a975a5013b2632083446b5220205ad16057b1ef0` (the first,
over USB) and `30cdefb47286f2e3397dd13eaaba88fa772910ca41578a99baaf271adab993a4`
(SD card airgap), and
`f7f6ad4b01be8650c9dc5f78e7d844133e5f53c4daf6b3cc1131a802bae2f2cb` (the first
compact PCZT, 1,430 B instead of ~2.8 KB).

### Which link to use

- **USB or Bluetooth**, in a place you trust: fast (a few seconds) and
  reliable. The session is encrypted and the board shows every output before
  it signs, so the link carries nothing it could change unseen.
- **QR** keeps the board off every cable and radio, but it is slow: a send
  takes about 28 frames at 50 bytes each, and the camera needs steady light.
  Use it when that isolation matters more than speed.
- **SD card**: also fully offline, for the PC companion.

## Hardware

- An **ESP32-S3-N16R8 "CAM" board** (16 MB flash, 8 MB PSRAM, OV5640 camera,
  microSD slot).
- A **1.5" SH1107 128×128 I2C OLED**.
- An **EC11 rotary encoder**: turn to move, press for OK. Its direction is
  switchable on the board under Settings → Knob direction.
- **One Cancel button** (K3, on GPIO14).

That is the whole input: turn, press, Cancel. (The original four-button
module is optional and still works on the same pins.)

Wiring: [firmware/docs/WIRING-S3CAM.md](firmware/docs/WIRING-S3CAM.md).
Everything runs at 3.3 V.

## Repository

| Path | What |
|---|---|
| `firmware/` | Device firmware (ESP-IDF 5.5 via PlatformIO), forked from LeekWallet with its history. Zcash code in `src/zcash-*.c`, the C signer in `components/zsign-c`, the Rust signer in `components/zsign-rust` |
| `firmware/sim/` | Host simulator and tests: the firmware's C code built for Linux with fakes for the hardware; `zdev`, a simulated device on a PTY |
| `app/` | The Zcash LeekWallet app (Tauri 2: desktop and Android): `src/` the screens, `packages/core` the board protocol, `transport-serial` / `transport-ble`, `zcash-wallet` the wallet over `zecz-core`, `near-intents` the 1Click swaps, `src-tauri` the shell |
| `host/` | Rust crates ([README](host/README.md)): [`zecz-core`](host/zecz-core/README.md), [`zecz-cli`](host/zecz-cli/README.md), [`leek-device`](host/leek-device/README.md), [`leek-android`](host/leek-android/README.md) |
| `chrome/` | The Chrome extension: `engine/` (Rust → WebAssembly), `extension/` (MV3), `engine-spike/` (the 9.1 study) |
| `.github/workflows/` | CI: host tests, the app on the three desktop systems with an Android check, the Chrome engine and extension; `release.yml` builds a tag's desktop bundles and extension into a draft release |
| `scripts/release-collect.sh` | Assembles a release: the draft's files, the firmware and the APK, `SHA256SUMS`; prints the sign and upload commands ([RELEASES.md](RELEASES.md)) |
| `vectors/` | Key and PCZT test vectors (valid, compact, mainnet, transparent, refused) and their generators ([README](vectors/README.md)) |
| `docs/` | Roadmap, journal, formats, mainnet checklist, browser engine study ([index](docs/README.md)) |
| `docs/JOURNAL.md` | What was done at each step, what broke, what was measured |

The Android app lives in its own repository,
[leekwallet-zcash-android](https://github.com/0xOucan/leekwallet-zcash-android)
(a fork of `zodl-android`, branch `leekwallet`). Clone it into `android/`,
which is ignored here.

## Releases

Each version is one GitHub release carrying every product: the firmware
(update and provision images), the desktop app (AppImage, .deb, .msi, .dmg),
the Android APK and the Chrome extension, with a `SHA256SUMS` signed by the
maintainer's SSH key. [RELEASES.md](RELEASES.md) says what each file is, how
to verify it, and how to rebuild it and compare. Prebuilt binaries from one
place are a trust risk; building from source is the recommended way.
What changed in each version, and its codename: [CHANGELOG.md](CHANGELOG.md).
The first is v0.1.0 "Sacbeh".

## Build and test

Toolchain details: [docs/TOOLCHAIN.md](docs/TOOLCHAIN.md). The Zcash
LeekWallet app builds as its [README](app/README.md) says; the
Android fork builds as [its README](https://github.com/0xOucan/leekwallet-zcash-android#readme)
says.

**Firmware** (PlatformIO):

```sh
cd firmware
pio run -e esp32s3cam                                   # build
pio run -e esp32s3cam -t upload --upload-port /dev/ttyACM0
```

**Simulator and tests** (Linux, no board needed):

```sh
make -C firmware/sim test      # firmware unit tests, both signers, refusal vectors
make -C firmware/sim zdev      # simulated device on a PTY, for zecz
cd host && cargo test          # zecz library and CLI, device client, Android native library
```

**The app** (needs Node 22, pnpm 9, and on Linux webkit2gtk-4.1; Android
needs the SDK and NDK; see [app/README.md](app/README.md) and
[app/ANDROID.md](app/ANDROID.md)):

```sh
cd app && pnpm install
pnpm tauri dev                 # run on this computer
pnpm tauri build               # .deb / AppImage, .msi, .dmg
pnpm android:release:arm64     # Android APK (see ANDROID.md)
```

**A testnet send with `zecz`:**

```sh
cd host && cargo build --release -p zecz-cli
Z="target/release/zecz --data-dir ~/zecz-device"
$Z device-init --port /dev/ttyACM0          # pair, import the board's account
$Z sync
$Z send --to <address> --amount 0.001 --zec --memo "hi" --compact --out tx.pczt
$Z sign-device --port /dev/ttyACM0 --in tx.pczt --out tx.zsig   # review on the board
$Z finish --pczt tx.pczt --sigs tx.zsig      # prove, apply signatures, broadcast
```

Every command: [host/zecz-cli/README.md](host/zecz-cli/README.md).

## Roadmap

The full plan, with a "done when" check for each step, is in
[ROADMAP.md](docs/zcash/ROADMAP.md); what happened at each step is in
[JOURNAL.md](docs/JOURNAL.md).

- [x] Firmware on the board: keys, storage modes, both signers, USB, Bluetooth, QR, SD
- [x] `zecz` CLI and the first board-signed testnet transactions
- [x] Android: import, USB / Bluetooth / QR signing, verify address on device
- [x] Compact PCZT on every link (Phase 7)
- [x] Desktop skeleton and CI on three systems (8.1)
- [x] Desktop: pair with the board, sign, verify address (8.3), wallet screens (8.4)
- [x] Firmware: transparent inputs, first shield signed on the board (10.1)
- [x] Chrome: a full Zcash engine runs in the browser (9.1)
- [x] Chrome extension engine, stored on OPFS, with a popup (9.2)
- [x] Mainnet ready on the PC, testnet kept as the default option (10.3)
- [x] First mainnet send and shield signed on the board (10.3)
- [x] Chrome extension with the board over Web Serial / Web Bluetooth (9.3), tested against the simulated board
- [x] Chrome extension on the real board over USB: a mainnet send signed and broadcast (9.3)
- [x] Chrome extension on the real board over Bluetooth (9.3)
- [ ] Chrome extension: software wallets (9.4)
- [ ] Firmware: several accounts, Bluetooth bonding, security review (Phase 10)
- [x] NEAR Intents swaps in the desktop app (8.6)
- [x] The board refuses requests for the other network (10.3)
- [x] Zcash in the LeekWallet Companion, desktop and Android build (6.14), on the real board next
- [x] The Companion is Zcash only: its EVM code, WalletConnect and the EVM extension removed
- [x] The first mainnet NEAR swap, ZEC → USDC on Base, signed on the board (8.6)
- [x] One app: the desktop app merged into Zcash LeekWallet (`app/`), NEAR swaps on Android too (6.6), data copied over on first start
- [ ] A NEAR swap from the Android tablet; NEAR Intents in Chrome
- [ ] A proper account type in the Android app and an offer to Zodl upstream
- [ ] Releases and the hackathon submission

## License

[Apache License 2.0](LICENSE). Copyright 2026 0xoucan. LeekWallet and bundled
third-party code keep their notices: see [NOTICE](NOTICE),
[firmware/NOTICE](firmware/NOTICE) and
[firmware/THIRD-PARTY-LICENSES.md](firmware/THIRD-PARTY-LICENSES.md).
