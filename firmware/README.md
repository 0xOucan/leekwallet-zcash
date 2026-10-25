# LeekWallet Zcash firmware

<p align="center">
  <img src="docs/leekwallet-logo.png" alt="LeekWallet" width="160"/>
</p>

Website: [zcash.leekwallet.xyz](https://zcash.leekwallet.xyz) · Demo: [2-minute video](https://www.youtube.com/watch?v=8F6_8AhsnAA) · X: [@gmLeekWallet](https://x.com/gmLeekWallet) · [@0xOucan](https://x.com/0xOucan)

The firmware of the LeekWallet Zcash board: a DIY Zcash hardware wallet on an
ESP32-S3. It holds the seed, checks every transaction it is asked to sign
against its own keys, shows you what you are signing, and signs it. The
companion (phone or computer) syncs, builds and proves; the keys never leave
the board. It signs Zcash only.

Start at the [top-level README](../README.md) for the whole project (apps,
status, roadmap). This file is about the board and its firmware.

> **Experimental, not audited.** Flash encryption and secure boot are not
> enabled (no eFuses burned), so anyone holding the board can read its flash.
> Use small amounts on mainnet, and see [AUDIT.md](AUDIT.md).

## What it does

- **Keys.** A BIP39 seed (12 or 24 words, generated on the board with
  optional dice, or imported), an optional BIP39 passphrase typed on the
  board, ZIP 32 accounts, unified addresses (Orchard and Ironwood receivers
  plus a transparent one), the seed fingerprint (ZFP) on screen.
- **Signing.** A PCZT signer (ZIP 374). The companion sends the compact
  signer view; the board parses it, checks that the spends are its own,
  recomputes the sighash, rebuilds the notes and the memo it encrypts, and
  pages every output, amount, fee and memo on its screen. On ACCEPT it returns
  a `.zsig`: RedPallas spend-authorisation signatures for Orchard and Ironwood
  spends and ECDSA ones for transparent P2PKH inputs (shielding), bound to the
  SHA-256 of the exact bytes it reviewed. Two signers, C (`components/zsign-c`)
  and Rust (`components/zsign-rust`), are cross-checked against each other.
- **Address check.** The companion sends the address it displays, even a
  rotated diversified one; the board proves it is its own and only then shows
  it for you to compare.
- **Network.** Mainnet or Testnet under Settings → Network. The board refuses
  any request for the other network and says so on its screen.
- **Links.** USB (USB Serial/JTAG) and Bluetooth LE for everyday signing, an
  encrypted session confirmed by a six-digit passkey on both screens. Animated
  QR (the board's camera in, its screen out) and the microSD card when the
  board must stay off every link.

## The board

| Part | |
|---|---|
| ESP32-S3-N16R8 "CAM" board | 16 MB flash, 8 MB octal PSRAM, OV5640 camera, microSD slot |
| 1.5" SH1107 OLED, 128×128, I2C | the UI is laid out for 128×64 and stretched 2× vertically to fill the panel; QR codes and the boot logo use the whole panel at their own scale |
| EC11 rotary encoder | turn to move (UP/DOWN), press for OK |
| One push button, K3 on GPIO14 | Cancel (BACK); held, it locks |

That is the whole input: every screen, setting and gesture works with a turn,
a press and Cancel. The original four-button module (UP, DOWN, BACK, OK on
GPIO1, 41, 14, 42) is optional and still works alongside or instead of the
knob; with it left off, GPIO1 and GPIO41 stay unconnected.

Turning the knob clockwise is DOWN (the next menu item, the next page) and
anticlockwise is UP. In the pickers (PIN digits, seed and passphrase
letters, the device name, the dice face) clockwise moves forward, so a PIN
digit goes up. If you prefer it the other way round, **Settings → Knob
direction** switches between Normal and Reversed; the change applies on the
next click of the knob, is kept across restarts, and affects the knob only.

The bottom line of each screen says what each input does there, in the
knob's own words: `X:` is the Cancel button, `TURN` is turning the knob and
`OK:` is pressing it. The PIN pad, for example, reads `X:DEL   TURN  OK:SEL`
and a transaction page `X:REJECT TURN OK:NEXT`. The four-button boards
(`esp32s3`, Pixie) keep their original footers (`UP DN  DEL  SEL`); the
wording is picked at build time from the board, in `src/hints.h`.

On power-on the board loops a logo animation, then `@gmLeekWallet`,
`v0.1.0 Sacbeh` and "the white road" with a blinking `Push the knob`, until the
knob is pressed (on the four-button boards, any key); turning it or Cancel
only jumps to the prompt. Left alone for 10 minutes the screen goes black
against OLED burn-in, and the next input only wakes it. The PIN unlock
screen, untouched for 30 s, falls back to the animation: the typed digits
are wiped, nothing is submitted, and the attempt counter is unchanged. The
version is set in one place, `src/version.h`, which `getFeatures` reports too.

Everything runs at 3.3 V. Wiring: [docs/WIRING-S3CAM.md](docs/WIRING-S3CAM.md);
the board's pins: [docs/BOARD-S3CAM-PINOUT.md](docs/BOARD-S3CAM-PINOUT.md).

The `esp32s3cam` build is the Zcash board. The `esp32s3` (the upstream
reference board: plain ESP32-S3, SSD1306, four buttons) build still has the Zcash signer but no camera or
card slot. The `pixie` build (Firefly Pixie, ESP32-C3) boots and holds seeds
but has no Zcash signer, which is built for Xtensa only
([docs/PIXIE-PORT.md](docs/PIXIE-PORT.md)).

## Storage modes

| Mode | Where the seed lives | Unlocked by |
|---|---|---|
| **Vault** | Flash (NVS), up to 30 seeds, each encrypted with AES-256-GCM under a key from the PIN (PBKDF2), with power-cut-safe writes and a wipe after too many wrong PINs | the PIN |
| **Stateless** (Temp Seed) | RAM only. Typed in each session and gone on lock or power-off; nothing is written | nothing to unlock: the words themselves |
| **SD card** | An encrypted `.zkv` file on the microSD card (format: [docs/zcash/FORMATS.md](../docs/zcash/FORMATS.md)), loaded for the session | its passphrase, never the PIN |

All three give the same keys for the same seed. Per-account metadata (the
wallet birthday per network, a label) is kept in flash only for a stored
wallet without a passphrase, and in RAM otherwise. More: [docs/VAULT.md](docs/VAULT.md),
[docs/BACKUP.md](docs/BACKUP.md).

## Build and flash

Needs [PlatformIO](https://platformio.org/) (`pip install platformio`); it
downloads ESP-IDF 5.5 and the toolchain on the first build. The Rust signer
needs the Espressif Rust toolchain: [docs/TOOLCHAIN.md](../docs/TOOLCHAIN.md).

```sh
cd firmware
pio run -e esp32s3cam                  # build
./flash-board.sh s3cam                 # flash the app only, keeps the wallet
./flash-board.sh s3cam --port /dev/ttyACM0 --fresh   # a blank board: bootloader, partitions, app
./monitor.sh                           # serial log (Ctrl-] to exit)
```

`flash-board.sh` asks the running firmware which board it is before
writing, because two ESP32-S3 boards cannot be told apart by the chip and
the wrong image drives the display and buttons on camera pins. Plain
`pio run -e esp32s3cam -t upload --upload-port /dev/ttyACM0` also works.

On Linux the board is `/dev/ttyACM0`; add yourself to the `dialout` group
(`sudo usermod -a -G dialout $USER`, then log in again).

On first boot: set a PIN, then create or import a seed, or choose Temp Seed
or the SD card. Settings → Network chooses Mainnet or Testnet.

Releases (images, checksums, reproducible builds) are in
[docs/RELEASE.md](docs/RELEASE.md) and [docs/SIGNING-KEYS.md](docs/SIGNING-KEYS.md);
what a release must carry for the Companion's flasher is in
[app/README.md](../app/README.md#firmware-releases). Secure boot and flash
encryption: [docs/BURN-PROCEDURE.md](docs/BURN-PROCEDURE.md) (never done on a
board yet).

## The protocol

Companions talk to the board over one protocol, on USB and Bluetooth alike:
length-prefixed CBOR frames, an X25519 session with commit-then-reveal and a
passkey compared on both screens, then ChaCha20-Poly1305. The Zcash calls are
`zGetFeatures`, `zGetAccount`, `zShowAddress`, `zSetAccountMeta` and
`zSignBegin` / `zSignChunk` / `zSignFinish`; `getStatus` reports the lock
state, the wallet, the account and the board's network. The specification
is [docs/PROTOCOL.md](docs/PROTOCOL.md); the `.zsig`, `.zkv` and PCZT
formats are in [docs/zcash/FORMATS.md](../docs/zcash/FORMATS.md).

Clients: `host/leek-device` (Rust, used by `zecz` and the Zodl fork on
Android), `app/packages/core` (TypeScript, the Zcash LeekWallet app) and
`chrome/extension/src/board` (the Chrome extension).

## Simulator and tests

No board needed. `sim/` builds the firmware's own C files for Linux with
fakes for the hardware ([sim/README.md](sim/README.md)):

```sh
make -C sim test        # unit suites: PIN, vault, UI, protocol, both signers, refusal vectors
make -C sim zdev        # zdev: the firmware's protocol on a PTY, a simulated board
make -C sim conformance # regenerate the vectors the Companion's mock is checked against
```

`zdev` answers the companions as the board would (`--network test|main`,
SIGUSR1/SIGUSR2 switch the network as Settings → Network does) and is what
the host, Companion and Chrome tests pair with. QEMU runs the real image
without a board: [docs/QEMU.md](docs/QEMU.md).

## Layout

| Path | What |
|---|---|
| `src/` | The firmware: UI, protocol, session, transports, PIN, entropy; Zcash in `zcash-*.c`, the SD mode in `sd-*.c` and `zkv.c`, the QR air gap in `airgap.c` and `ur*.c` |
| `components/leek-wallet/` | The vault |
| `components/zsign-c/`, `zsign-rust/`, `zsign-common/` | The two Zcash signers and their shared interface |
| `components/trezor-crypto/` | BIP39, BIP32, secp256k1 (transparent keys), SHA-2, HMAC, ChaCha20-Poly1305 |
| `components/esp32-camera/`, `esp_jpeg/`, `quirc/` | Camera and QR decoding |
| `sim/` | Host simulator, tests and zdev |
| `docs/` | Protocol, vault, wiring, release and audit documents |

## Credits and license

Forked from [LeekWallet](https://github.com/0xoucan/leekwallet), an
open-source ESP32-S3 hardware wallet for Ethereum by the same author, with
its history: the vault, PIN, entropy, session, transports, UI and the
simulator are LeekWallet's. *Lek* is Yucatec Maya for a container made from
a dried jícara gourd, made to keep something worth keeping.

Licensed under the [Apache License 2.0](LICENSE). Bundled third-party code
keeps its own licenses and notices: trezor-crypto, chacha20poly1305, QRCode,
quirc, esp32-camera, esp_jpeg and others, listed in
[THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md) and [NOTICE](NOTICE).
The [Colibri](https://github.com/xtools-at/colibri) wallet inspired the RPC
surface; it is AGPL-3.0 and none of its code is used here.

Thanks to [Trezor](https://github.com/trezor/trezor-firmware) for
trezor-crypto, [ricmoo](https://github.com/ricmoo/QRCode) for QRCode,
[SeedSigner](https://seedsigner.com/) for the stateless idea, and the
Zcash developers for librustzcash, Orchard and the PCZT format.
