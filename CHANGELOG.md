# Changelog

Each release is one tag in this repository; its files, checksums and how to
verify or rebuild them are in [RELEASES.md](RELEASES.md). The Zodl fork's APKs
are released in
[leekwallet-zcash-android](https://github.com/0xOucan/leekwallet-zcash-android)
(LEEKWALLET-RELEASE.md there).

## Unreleased

- **Chrome extension: QR (built, not yet tested on the board).** The
  extension now reaches a board kept off every link, as the desktop app
  does. *Sign with QR* in the Send and Shield review shows the compact
  signer view as an animated `ur:leek-pczt` in a tab on a black page
  (leek-ur's display rules; *Laptop screen* 50 B / 300 ms by default,
  *Smaller codes* 40 B, *Safe* 50 B / 500 ms; full screen), then reads the
  board's `ur:leek-zsig` through the webcam and hands it to the engine's
  `finish`: every signature checked before it is applied, proved, stored,
  then broadcast. *Import from the board's QR* reads the board's View key
  (`ur:leek-zcash-accounts`) with no cable; such an account is shown
  without its board and can be forgotten without it. *Check on the board
  by QR* shows the receive address for the board's Check address.
  `host/leek-ur` runs in the page as a new small WASM module
  (`chrome/ur`), apart from the engine, which a sync holds for minutes.
  New pinned dependencies, vendored at build time: `qrcode-generator`
  2.0.4 and `zxing-wasm` 3.1.3 (the desktop app's versions; zxing's CDN
  default replaced by the packaged `.wasm`). No new permission: the
  webcam is asked for in the tab. Tested in Node (codes drawn and read
  back) and in headless Chromium with a stand-in webcam
  (`test/e2e.mjs` step 10). QR is never used for pairing.

- **QR signing is no longer experimental.** It signed testnet sends in
  both directions from `zecz`, the desktop app and the Android app on a
  phone, and from our Zodl and Zingo forks. The "experimental" label is
  gone from the app's buttons, `zecz`'s help and pages, the SDK's
  `address_check_hint()` (now "On the board: Main menu → Check address,
  then scan this code.") and the docs; that covers *Sign with QR*, the
  View key import and Check address. Check address is new and not yet
  tested on the board. QR is still never used for pairing. The SD card,
  the QR screen's filled layout and the OPTIGA work keep their labels.
- **Brightness is the user's on the QR page (QR signing).** A
  user test on an OLED phone (2026-11-11) found forcing 100 % was the
  blocker: the board found the code but read it washed out. The SDK's
  `qr_display_hints()` now has `brightness_levels` ([system, 0.6, 0.8,
  1.0]; `None` = system) and `default_brightness` (`None`); `max_brightness`
  stays but is always false. The phone tips say to adjust brightness on the
  QR page when the board finds the code but does not read it (lower first
  if it looks washed out, higher if dark). The companion on Android has a
  *Brightness* button inline and in full screen (system → 60 % → 80 % →
  100 %, remembered, hidden on the desktop), and `qr_screen(on, level)`
  keeps the screen on at that level instead of 100 %.
- **Check address over QR.** SDK: `address_check_qr(address)`
  (the plain address to show, from an address or a `zcash:` URI) and
  `address_check_hint()`, for the board's Main menu → Check address, which
  says whether the address is its own, with the account and diversifier
  index. The companion's Receive view has *Check on board (QR)* for both
  addresses. `leek-zcash-ffi`'s README has "Verifying a receive address"
  (USB/Bluetooth `show_address`, or QR).
- **Board: Check address (QR; new, not yet tested on the board).** Main menu → Check address
  scans a wallet's Receive QR (a unified, Sapling, transparent or TEX
  address, or a one-payment `zcash:` URI) and says from the board's own
  keys whether it is this wallet's: *YOURS, account N, address #index* with
  the receivers checked, *NOT from this wallet*, *Cannot check*, or *Wrong
  network*, then the address in groups of four to compare. A unified
  address's index comes from decrypting its Orchard diversifier, and its
  transparent receiver has to match at that index, on any of accounts 0–9;
  a transparent address is searched in the selected account's #0–199. The
  board has no Sapling keys: a Sapling receiver is not checked and the
  screen says *Sapling NOT checked*; a Sapling-only address is *Cannot
  check*. The QR-only
  counterpart of the USB/Bluetooth address check
  ([QR-BENCH.md](docs/zcash/QR-BENCH.md#check-address-is-the-phones-address-the-boards)).
- **QR scanning on the board: 2–3x faster, and phones work (tested on the
  colour kit, [QR-BENCH.md](docs/zcash/QR-BENCH.md#signing-with-a-phone)).**
  The scan decoder keeps 64 mixed parts instead of 8 and skips one it already
  holds; a full queue had turned most reads into repeats. quirc's grid fit
  does its maths inline, bit-identical (a boot self-test checks 80,000
  cases), which cuts the code search for a v8 code from about 290 ms to
  120–190 ms a frame. Laptop screen at 2x: 13–20 s for 39–88 B fragments,
  where 88 B did not decode before. The Scan screen opens at 2x and keeps the
  zoom the knob chose until power-off. First phone signature by QR both ways: 25 B at
  350 ms, 3x, about 30 cm (the lens is fixed-focus). The `-qstages` and
  `-qfast` images log quirc's stage times and why codes fail.
- **SDK: `qr_display_hints()`, how to show the signing QR.**
  Next to `qr_defaults()` in `leek-zcash-ffi` (Kotlin, Swift), from
  `host/leek-ur`'s `display` rules: 25 B at 350 ms from a phone and 50 B at
  300 ms from a laptop or tablet, the screen kept on,
  a black page with a 2-module quiet zone, error correction L, uppercase
  alphanumeric, and short tips: anti-flicker / DC dimming on OLED phones
  (PWM dimming shows as bands to the board's rolling shutter, and it
  improved reads a lot), the board at 3x about 30 cm for
  a phone, 2x for a laptop or tablet. `qr_defaults().fragment_choices` now
  offers 25 B. `build-android.sh` takes `--out <dir>` or
  `LEEK_ANDROID_OUT`, so integrators need not write into `host/target`.
- **Companion on Android: the screen kept on while the Sign-with-QR code
  shows** (and in full screen), restored after; the SDK's tip under the
  code. The desktop is unchanged. (It first forced full brightness; see
  "Brightness is the user's" above.)
- **Companion: big codes and a black full screen for phones.** A *Big codes*
  preset (25 B, QR v4), *Bigger* down to 25 B, and a black full-screen page
  with a 2-module white border and a brightness and glare hint.

- **CAM kits: camera tuning for faster QR scanning (measured on the colour
  kit, [QR-BENCH.md](docs/zcash/QR-BENCH.md)).** The AGC gain ceiling was effectively 1/16x (the driver wrote its
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
- **Seed backups in the secure element and back (development image only).**
  A new env, `esp32s3cam-color-se` (colour kit + OPTIGA Trust M on GPIO47/21),
  adds Settings > Secure element: copy the open seed into the chip's Level 1
  container, compare it, restore it to the session or to the vault, erase it,
  and restore an SD `.zkv` file into the chip or into the vault. Every action
  asks for the PIN first. The copy is sealed with the vault's own KDF and
  AES-256-GCM under the PIN, with a salt of its own so it opens on a rebuilt
  vault; screens show only its ZFP. Restoring into the vault never overwrites
  or deletes a seed already there: the same seed is a no-op, another goes into
  a new slot, a full vault refuses. The chip is left unlocked, so this copy is
  a backup location, not extra security; the vault stays the master
  (`firmware/docs/BACKUP.md`). Every other image is unchanged. Host-tested
  (`sim/test_se_backup.c`); not yet run on a chip.
- **The QR air gap works on the board.** A board kept off
  USB and Bluetooth signs by animated QR in both directions: the request as
  `ur:leek-pczt`, the reply as `ur:leek-zsig`. Three testnet sends were
  signed this way on the colour kit; the first, by `zecz`, was mined in
  block 4,606,736 (`8f3b9264…3c7d`). QR is never used for pairing.
  - **Desktop app:** *Sign with QR* on the Send and Shield review. It
    defaults to 50-byte fragments at 500 ms (Speed: Safe 50/500, Balanced
    50/400, Fast 50/300, Custom). A full-screen view shows the code as large
    as the window allows. A stopwatch, a manual step mode and a *Bench
    sweep* through 11 settings produce a Markdown table. The reply decodes
    in about 7 s instead of 39 s.
  - **Bench:** a compact send took 60.5 s to read at 40 B and 70.6 s at
    50 B, with the board at 2x zoom
    ([QR-BENCH.md](docs/zcash/QR-BENCH.md)).
  - **`zecz`:** `send --qr` and `--qr-html`, `qr-show` (with `--sweep`) and
    `qr-scan`, from the webcam with the `qr-camera` feature or from a file
    of parts.
  - Both use one new crate, `host/leek-ur`, which produces the firmware's
    parts byte for byte.
- **View key: set up a wallet without connecting the board.**
  On the board, Main menu → View key: a warning ("Whoever scans it sees
  balance and history. It cannot spend."), then the account as an animated
  `ur:leek-zcash-accounts`. It carries the seed fingerprint, the UFVK and,
  when the board knows them, its label and birthday height
  ([FORMATS.md](docs/zcash/FORMATS.md#account-record)). The desktop app
  imports it as a board wallet under Wallets on this device → *Import from
  the board's QR*, with the board's birthday filled in. This was tested
  with the board unplugged: testnet account 0 imported at block 4,506,700
  and synced.
- **The board's QR screen (colour kit).**
  - A turn sets the backlight live. A push shows the same code filled to
    the panel, then the next density, plain: still, v6, v4, v3, v9. The
    filled layout is experimental: one module of quiet zone and modules a
    pixel apart in size, v6 at 228 px instead of 205.
  - Receive shows the Orchard-only address, a smaller code (v5 on mainnet).
  - The scanner follows a sender that switched messages after three of its
    parts in a row, and logs why it refused a part.
- **SDK: animated QR for other wallets.** `leek-zcash-ffi` (Kotlin and
  Swift) gains `QrScanner`, which takes camera texts and returns the View
  key's account record or the board's `.zsig`; it refuses a key for the
  other network and follows a switched sender. It also gains `QrEncoder`
  (`ur:leek-pczt` frames) and `qr_defaults()` with the bench's settings. No
  wallet ships it yet.
- **SDK: ready for Android wallets.**
  - `leek-zcash-ffi/scripts/build-android.sh` builds the library for
    arm64-v8a and x86_64 with 16 KB page alignment, which Android 15 needs.
    It refuses bindings that don't match the library, and it ships R8 and
    JNA keep rules.
  - New tests: a PCZT built the way the Zodl SDK builds one passes
    `preflight` and signs on the simulated board, and the SDK matches the
    Zodl fork's JNI library byte for byte.
  - New `firmware_outdated()`.
  - When the board refuses the network, the error now says which network
    the board is on: `BoardNetwork`, still code 400.
  - Nothing has run on a phone yet.
- **Secure element research, and a development-only SE container.** How
  Trezor and BitBox use the OPTIGA Trust M, and a design for using it here
  (`firmware/docs/RESEARCH-SECURE-ELEMENT.md`,
  `firmware/docs/RESEARCH-OPTIGA-WALLETS.md`). The driver can now store one
  blob in data object `0xF1E0`, a reversible test of the write path that adds
  no security, behind `LEEK_SE_CONTAINER_WRITES`. The flag is off by default,
  so every image, the bench included, stays read-only. The container is not
  wired to the wallet, and nothing has been written to any chip.
- **Colour kit: a real camera view, and a backlight setting.** Scan showed
  the OLED's 1-bit preview stretched to a few bars; it is now a 200×150
  grayscale picture, auto-levelled per frame, with rows for the exposure
  (`Light <=O----> -4 23%`), capture size, frames/s, codes located and read,
  a progress bar for animated URs, and fixed hints (Too dark, Too bright,
  Hold steady, Move closer). **Settings → Preview** shows the true exposure
  instead. **Settings → Screen** dims the backlight (PWM on GPIO43, the
  header's TX): move the module's BLK from 3V3 to TX to use it
  (WIRING-S3CAM-COLOR.md). The steps are 1, 2, 3, 5, 7, 10, 15, 20 % and
  tens, and below 1 % fine ones down to a single tick of the 14-bit PWM
  (0.7, 0.5, 0.3 … 0.02, 0.01, 0.006 %), because even 1 % was far too
  bright for a webcam reading the panel's QR. One level for every screen,
  QRs included: Receive and the signature QR keep their knob (Receive pages,
  the QR steps its module size). The OLED kits are unchanged.
- **Colour kit: QRs fill the panel.** Every QR (Receive, a signature) is
  drawn from its modules at the largest whole number of pixels per module
  that leaves two modules of quiet zone, with the light running to the
  panel's edges: a t-address is 7 px a module instead of 6, a v6 code 5
  instead of 4. The signature QR has its own sizes on this panel (v9 at
  4 px, v6 at 5, v4 at 6, v3 at 7; a turn or OK steps them) and opens as one
  still code where the message fits in one at 4 px, else at v6 5 px. A small
  line under the code says the mode, the frame and the backlight where it
  fits without shrinking the modules.
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
