# Changelog

Each release is one tag in this repository; its files, checksums and how to
verify or rebuild them are in [RELEASES.md](RELEASES.md). The Zodl fork's APKs
are released in
[leekwallet-zcash-android](https://github.com/0xOucan/leekwallet-zcash-android)
(LEEKWALLET-RELEASE.md there).

## Unreleased

- **CAM kits: camera tuning for faster QR scanning (not yet measured on a
  board).** The AGC gain ceiling was effectively 1/16x (the driver wrote its
  enum value into a real-gain register), so all brightness came from shutter
  time and blur; it is now a real 4x. Banding is 60 Hz (was 50 Hz). The
  shutter is capped at 96 lines and metering is centre-weighted, with the
  knob's -5..0 bias kept. The knob push now zooms on the sensor, 1x → 2x → 3x
  (2.67x) → 1x, always captured at QVGA and opening at 1x; VGA and SVGA are
  gone from the Scan screen and stay in the bench image. Row 6 shows the zoom
  (`1x 12fps s:3 r:1`), and the colour kit shows the light level's percentage
  in green, amber or red. The CPU runs at 240 MHz only while scanning (160
  otherwise), and decoding runs on core 1. Once a second the serial log shows
  per-stage times, exposure, gain and the CPU clock, and each animated UR logs
  its time to complete. Every change has a build define to switch it off;
  JOURNAL.md "Camera tuning" has the board checklist. Delete
  `firmware/sdkconfig.esp32s3cam*` before building.
- **Colour kit: a real camera view, and a backlight setting.** Scan showed
  the OLED's 1-bit preview stretched to a few bars; it is now a 200×150
  grayscale picture, auto-levelled per frame, with rows for the exposure
  (`Light <=O----> -4 23%`), capture size, frames/s, codes located and read,
  a progress bar for animated URs, and fixed hints (Too dark, Too bright,
  Hold steady, Move closer). **Settings → Preview** shows the true exposure
  instead. **Settings → Screen** dims the backlight (10–100 %, PWM on GPIO43,
  the header's TX): move the module's BLK from 3V3 to TX to use it
  (WIRING-S3CAM-COLOR.md). The OLED kits are unchanged.
- **Colour kit: the screen stayed dark with Bluetooth on.** The colour build
  ran out of internal RAM before the UI task started ("Failed to create UI
  task"). The panel's buffers are smaller, its SPI interrupt code no longer
  sits in internal RAM, and the Zcash worker's stack is 16 KB instead of 24 KB
  on every board. The boot log now shows the internal RAM left for the UI, and
  the worker logs its lowest free stack after each job.
- **Colour kit: the screen, the knob and a crash.** The panel needs its CS
  line: on GND it stays black, so CS goes to GPIO48 (the onboard RGB LED's
  pin, unused) and the driver uses SPI mode 0. The module's encoder could not
  pull RX/TX low, so on the colour kit it moves to GPIO3 and GPIO46, wired
  straight with no resistor (strapping pins, harmless without eFuses burned or
  BOOT held). The button task's stack grows from 2 KB to 3 KB: a log call from
  it could overflow and restart the board. Bring-up aids: the
  `esp32s3cam-panel-bench` env (bit-banged panel test, no wallet) and
  `-DLEEK_ENC_TRACE=1` (logs every encoder edge).
- **NU7 on mainnet** (block 3,543,405, about 02:20 UTC on 2026-11-06). Every
  host (the app, `zecz`, the SDK, the Chrome engine, the Android library) moves
  from a librustzcash git rev with no mainnet height to the released crates
  that set it: `zcash_primitives 0.31.0`, `zcash_client_backend 0.25.0`,
  `zcash_client_sqlite 0.23.0`, `pczt 0.10.0` and siblings. An app without this
  builds transactions nodes reject after the activation. Rust 1.89 or newer.
- **New firmware: boards should be updated.** Tested on a board: a NU7 testnet
  send over USB, signed on the colour kit and mined in block 4,606,165
  (txid `8676ee562b8066b8…`). v0.1.0 still signs NU7 in the simulator, so it is a
  suggestion and not a requirement.
  The PCZT format did not change.
- **Version notices, both ways** (guidance, not a security boundary). Apps and
  `zecz` suggest updating a board that reports no `fwCompat` level. The board says
  "Update your app" when a request has no `hostCompat`. It also refuses a NU6.3
  PCZT with expiry at or above NU7 + 40, which only an app without NU7 builds:
  error `0x0519`, `ZS_E_APP_OUTDATED`, SDK `AppOutdated` 520.
- **Wallet databases do not change**: `zcash_client_sqlite 0.23.0` has the same
  migrations as the rev v0.1.0 used.
- The apps sync every 25 s (one NU7 block), new wallets start 288 blocks below
  the tip when the board has no birthday, and the review shows the network
  upgrade and the real expiry height (120 blocks from NU7, 40 before).
- A transaction signed before an activation is refused with a clear message
  before proving and before each broadcast (`check_broadcastable` in zecz,
  `check_broadcast` in the SDK, new errors `WrongBranch` 518 and `Expired` 519).
- The colour screen's boot logo now has the website's colours: a bright green
  pot and lid, the key turning pale as it glows, gold sparkles and a dim gold
  halo round the key. The OLED boards show the same picture as before.

## v0.1.0 "Sacbeh" (unreleased)

*Sacbeh*: from the Maya *sak bej*, "white road", the raised limestone
causeways that joined Maya cities across the jungle.

The first release, built during ZECATHON on top of the author's earlier
LeekWallet for Ethereum (tag `pre-zecathon`), with every EVM part removed.

- **Firmware** for the ESP32-S3 CAM board: a PCZT (ZIP 374) signer for
  Orchard and transparent inputs that checks every spend is its own,
  recomputes the sighash, shows each output, amount and memo, and returns
  only signatures. It verifies addresses on its own screen and refuses
  requests for the other network. Links: USB and Bluetooth (encrypted,
  confirmed by a six-digit code), and the SD card.
- **Zcash LeekWallet**, one app for Linux, Windows, macOS and Android
  (64-bit phones only): several wallets, mainnet or testnet, contacts, memos,
  QR pay and NEAR Intents swaps, every send signed on the board.
- **On the board**: a looping boot animation (the LeekWallet logo,
  `@gmLeekWallet`, `v0.1.0 Sacbeh`, "the white road") until the knob is
  pushed; an untouched PIN screen falls back to it after 30 s, and the
  screen goes dark after 10 minutes. Menus fill the 128x128 screen (text
  2x tall), on-screen hints speak knob and Cancel (`X:BACK TURN OK:SEL`),
  and Settings has Knob direction. The kit is the OLED, an EC11 knob and
  one Cancel button.
- **Chrome extension**: the same wallet in the browser, over USB or
  Bluetooth.
- **Releases**: `SHA256SUMS` signed with the maintainer's SSH key
  ([allowed_signers](allowed_signers), the key at
  <https://github.com/0xOucan.keys>). The firmware rebuilds byte-identical
  from the tag.

Not in this release: the firmware flasher inside the desktop app (flash from
the website's flasher or with esptool). Not audited; no secure boot or flash
encryption, so small amounts only.
