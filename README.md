# LeekWallet Zcash

[![desktop](https://github.com/0xOucan/leekwallet-zcash/actions/workflows/desktop.yml/badge.svg)](https://github.com/0xOucan/leekwallet-zcash/actions/workflows/desktop.yml)

A DIY Zcash hardware wallet on an ESP32-S3 board that costs about as much as
lunch. It holds the seed, checks every transaction against its own keys, shows
you what you are signing, and signs it. Your phone or computer builds and proves
the transaction; the keys never leave the device.

Built on [LeekWallet](firmware/README.md), an open-source ESP32 hardware wallet,
for Zcash's shielded pools: **Ironwood** (NU6.3), **Orchard**, and transparent
outputs.

> **Status: experimental.** It has signed real testnet transactions over USB,
> Bluetooth, QR and SD card. It is not audited. Flash encryption and secure boot
> are deliberately not enabled (no eFuses burned), so anyone holding the board
> can read its flash. Do not put real funds on it.

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

## Status

| Component | State |
|---|---|
| **Board + firmware** | Done and tested on real hardware: signs shielded and transparent spends (shielding) over USB, Bluetooth, QR and SD; compact PCZT; verifies any of its addresses |
| **Android app** ([repo](https://github.com/0xOucan/leekwallet-zcash-android)) | Imports the board's account; testnet sends signed over USB, Bluetooth and QR; "Verify on device" on the Receive screen |
| **`zecz` CLI** | Full flow on testnet: pair, import, sync, send (compact), sign on the board, prove, broadcast; `.zkv` SD cards |
| **Desktop app** (`desktop/`) | Pairs with the board over USB and Bluetooth, imports its account, syncs, verifies addresses on the board, sends and shields signed on the board (tested on the board); receive QR with rotating addresses, history, contacts. Builds on Linux, Windows and macOS in CI |
| **Chrome extension** (`chrome/`) | Engine done: librustzcash in WebAssembly in an extension worker, wallet stored on OPFS (survives restarts), gRPC-web sync, the whole send path in its API; a popup with network switch, import, sync, balances, address. Board link and software wallets next ([chrome/README.md](chrome/README.md)) |
| **Mainnet** | Ready on the PC: mainnet vectors and tests, servers of the wrong chain refused, a Testnet/Mainnet switch in the desktop app with a real-funds confirmation. Testnet stays the default. First real send: [MAINNET.md](docs/zcash/MAINNET.md) |
| **NEAR Intents** | Planned, after the first mainnet send |

## What works

| | |
|---|---|
| Keys | BIP39 seed + optional passphrase entered on the device, ZIP 32 accounts, unified addresses, seed fingerprint (ZFP) |
| Storage | PIN vault in flash, stateless (seed typed each boot), or an encrypted `.zkv` file on the microSD card |
| Signing | Ironwood and Orchard spends, and transparent P2PKH inputs (shielding), to shielded or transparent outputs, with memos. Two independent signers: C (on the device) and Rust (host and QEMU), cross-checked against each other |
| Links | USB serial and Bluetooth LE for everyday signing; animated QR (UR fountain codes, the board's camera) and the SD card when the board must stay off every link |
| Screen | 128×128 SH1107 OLED, EC11 rotary encoder + buttons; receive address as text and QR |
| Address check | The app sends the address it displays, even a rotated diversified one; the board proves it is its own (Orchard `pk_d = [ivk]·g_d`, index recovered by FF1 decryption, transparent receiver checked at that index) and only then shows it to confirm. Foreign or mixed addresses are refused |
| Companions | `zecz` CLI (PC), LeekWallet Zcash for Android (a [Zodl](https://github.com/zodl-inc/zodl-android) fork), and a desktop app in progress |

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
- An **EC11 rotary encoder** and a small four-button module.

Wiring: [firmware/docs/WIRING-S3CAM.md](firmware/docs/WIRING-S3CAM.md).
Everything runs at 3.3 V.

## Repository

| Path | What |
|---|---|
| `firmware/` | Device firmware (ESP-IDF 5.5 via PlatformIO), forked from LeekWallet with its history. Zcash code in `src/zcash-*.c`, the C signer in `components/zsign-c`, the Rust signer in `components/zsign-rust` |
| `firmware/sim/` | Host simulator and tests: the firmware's C code built for Linux with fakes for the hardware; `zdev`, a simulated device on a PTY |
| `host/` | Rust crates ([README](host/README.md)): `zecz-core` (the wallet engine), `zecz-cli` (`zecz`), `leek-device` (the board's client and `.zsig`), `leek-android` (JNI library for the app) |
| `desktop/` | LeekWallet Zcash desktop app (Tauri 2) on `zecz-core`, for Linux, Windows and macOS |
| `chrome/` | The Chrome extension: `engine/` (Rust → WebAssembly), `extension/` (MV3), `engine-spike/` (the 9.1 study) |
| `.github/workflows/` | CI: host tests, desktop builds on the three systems, the Chrome engine and extension |
| `vectors/` | Key and PCZT test vectors (valid, compact, mainnet, transparent, refused) and their generators ([README](vectors/README.md)) |
| `docs/` | Roadmap, journal, formats, mainnet checklist, browser engine study ([index](docs/README.md)) |
| `docs/JOURNAL.md` | What was done at each step, what broke, what was measured |

The Android app lives in its own repository,
[leekwallet-zcash-android](https://github.com/0xOucan/leekwallet-zcash-android)
(a fork of `zodl-android`, branch `leekwallet`). Clone it into `android/`,
which is ignored here.

## Build and test

Toolchain details: [docs/TOOLCHAIN.md](docs/TOOLCHAIN.md).

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

**Desktop app** (needs Node 22, pnpm 9, and on Linux webkit2gtk-4.1; see
[desktop/README.md](desktop/README.md)):

```sh
cd desktop && pnpm install
pnpm tauri dev                 # run
pnpm tauri build               # .deb / AppImage, .msi, .dmg
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
- [ ] First mainnet send signed on the board
- [ ] Chrome extension: the board over Web Serial / Web Bluetooth (9.3), software wallets (9.4)
- [ ] Firmware: several accounts, Bluetooth bonding, security review (Phase 10)
- [ ] NEAR Intents swaps signed on the board, on Android, desktop and Chrome
- [ ] A proper account type in the Android app and an offer to Zodl upstream
- [ ] Releases and the hackathon submission

## License

[Apache License 2.0](LICENSE). Copyright 2026 0xoucan. LeekWallet and bundled
third-party code keep their notices: see [NOTICE](NOTICE),
[firmware/NOTICE](firmware/NOTICE) and
[firmware/THIRD-PARTY-LICENSES.md](firmware/THIRD-PARTY-LICENSES.md).
