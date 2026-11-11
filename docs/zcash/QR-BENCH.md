# QR signing bench (experimental)

QR signing is experimental. USB and Bluetooth are still the everyday links.
QR is never used for pairing: USB and Bluetooth always ask for the 6-digit
code. This page says how fast the board reads a `ur:leek-pczt` loop and how
fast a host reads the board's reply, with the settings that worked.

## Recommended settings

From the bench of 2026-11-10 on the colour kit (OV5640, 240×240 panel).
Each send was a compact NU7 testnet send of 1,373 B:

| Direction | Setting | Result |
|---|---|---|
| Host screen → board | **50 B fragments, 500 ms a frame** (the default: the app's Safe preset, the SDK's `qr_defaults()`) | 70.6 s (58 parts for 28 fragments) |
| Host screen → board | 40 B, 500 ms | **60.5 s**, the fastest so far (36 parts, QR v5) |
| Host screen → board | 30 B, 500 ms | 83.0 s: also a v5 code, so smaller fragments do not help |
| Host screen → board | 90 B, 500 ms | the board found the v8 code 253 times and decoded it 3 times; the app gave up after 81 s |
| Board → laptop webcam | the board's default mode | 7.4 s (31 codes read) |

**On the board:** 2x zoom (push the knob on the scan screen) and exposure
-4. The Scan screen opens at 1x after power-up and then keeps the zoom the
knob last chose until power-off.

### Since 2026-11-11 (decoder and code-search fixes)

Two firmware changes made every setting faster:

- The decoder keeps 64 mixed parts instead of 8, and skips a mixed part it
  already holds. A full queue used to drop useful parts, so most reads were
  repeats (30 to 100 a scan). After the fix, 4 to 18.
- quirc's grid fit does its maths inline (the same results bit for bit; the
  board's boot self-test compares 80,000 cases). The code search for a v8
  code went from about 290 ms to 120–190 ms a frame.

From the laptop screen at 2x, with the board's LEDs covered:

| Setting | Board read |
|---|---|
| 39–40 B, 250–350 ms | 15–19 s |
| 49 B, 200–350 ms | 13.5–20 s |
| 88 B (QR v8), 200–350 ms | 13–16 s (it did not decode at all before) |

At 200 ms the screen changes faster than the board processes frames (about
5 a second with a code in view), so it gains nothing. 250–350 ms is the
range to use. A faster sensor setting (VTS 700) was tried and did not help:
the CPU is still the limit, and more codes failed to read.

**From the board:** the board can't tell a phone from a webcam, so you pick
the density on the knob (below). A laptop webcam reads the default (v6). A
phone held close can read the denser v9.

## Signing with a phone

It works, but the phone needs other settings than a laptop screen:

- **Distance and zoom.** The camera has a fixed-focus lens ("this module
  has no autofocus" in the boot log), sharp from about 30 cm. At 10–15 cm
  the board found the phone's code 153 times and read it 4 times. Hold the
  phone about 30 cm away and push the knob to **3x**.
- **Bigger codes.** In the app, *Big codes* (25 B, QR v4), full screen. The
  full-screen page is black, with a 2-module white border round the code.
- **Brightness high.** At half brightness the board ran at its longest
  exposure with 4x gain and the picture was noisy.
- **The board's LEDs.** The power LEDs on the board and the OPTIGA module
  are wired to the supply, so firmware cannot turn them off. Their light
  reflects in a phone's glossy screen straight back into the camera. Cover
  them with black tape, or use an opaque case.

Tested on 2026-11-11 (Motorola edge 50 neo, the companion's debug APK): 25 B
at 300–350 ms, 3x, about 30 cm. The board read the request in 38.7 s (91 s
on the app's clock, aiming included), and the phone read the board's
one-code reply in 9.1 s. The other way, the phone read the View key in
about 5 s at the densest setting.

## The board's QR screen (colour kit)

- **Turn** the knob to change the backlight. The level is saved when the QR
  closes.
- **Push** to change the code. Each density is shown first as it is, then
  filled to the panel, then the next density starts. The order is still →
  v6 → v4 → v3 → v9, then round again.
- **Plain** layout: whole pixels per module, at least 2 modules of quiet
  zone, and a caption underneath (`v6 5px 2/9 0.3%`). It uses 77–88 % of
  the 240 px panel (v3 203 px, v4 198, v5 185, v6 205, v9 212).
- **Filled** layout (experimental): 1 module of quiet zone, with modules of
  a fractional size, so neighbours differ by one pixel. It uses 93–96 % of
  the panel (v6 228 px, v7 229 px), and there is no caption. The serial log
  says `QR out: v6 fill`. zbar reads it in the simulator; on a board it
  still has to be measured against a webcam and a phone.

The same screen shows the signature reply (`ur:leek-zsig`) and the View key
(`ur:leek-zcash-accounts`).

## View key: the account by QR

On the board: Main menu → View key. A warning says that anyone who scans it
sees the balance and history, and that it cannot spend. After OK, the board
shows the account as an animated `ur:leek-zcash-accounts`. The record has
the seed fingerprint and the UFVK with its account index. It also carries
the label and the birthday height when the board knows them, which it does
once a companion has set them over USB or Bluetooth
([FORMATS.md](FORMATS.md#account-record)).

The desktop app imports it as a board wallet: Wallets on this device →
Import from the board's QR. It derives the address from the UFVK and fills
in the board's birthday. Without a birthday, an empty field means a new
seed (scan from near today's tip). For an older seed you enter the height
it was created at.

Tested on 2026-11-11: the colour kit's testnet account 0 (ZFP 551A1FD4) was
imported with the board's birthday (block 4,506,700) and synced to the tip,
with the board unplugged. The SDK's `QrScanner` does the same for other
wallets ([leek-zcash-ffi](../../host/leek-zcash-ffi/README.md)).

## How to measure

1. Build a testnet send: the app's review, or `zecz send ... --out tx.pczt`.
2. Show it: in the app, *Sign with QR* → *Bench sweep*. It steps through 11
   settings and copies a Markdown table. Or run
   `zecz qr-show --in tx.pczt --sweep`.
3. On the board, choose *Scan*. Time from the first frame until the board
   says the scan is complete. The board's log line `scan:` gives `s:`
   (codes located) and `r:` (parts read).
4. Cancel on the board before the next setting: a new fragment size is a
   new message to its decoder. The board follows a sender that switched
   after three parts of the new message in a row.
5. Note the board's zoom and exposure and the screen's brightness. The app's
   stopwatch (⏱) logs `zcash: QR bench ⏱ …`.

## Transactions signed by QR

| Txid | How | Block |
|---|---|---|
| `8f3b9264fddce48f6989396336741512e3fdafe66870826c0ceeb63725e73c7d` | `zecz`, the first send by QR in both directions | 4,606,736 |
| `43f6c901675c3d4c2f4067f84d1db238c10ab6059d20bcea3f16c631d19ac085` | the desktop app | testnet |
| `859266116ab58e55eaa255041ca22e727f0507ce90cc3b7b1dd2e9f9381e19e9` | the desktop app, faster reply decode (67.9 s end to end) | testnet |
| `a21ec09bfbd98c19db10aadfcc15ef576b72cb7b01e70582744863c009c848dc` | the desktop app, 50 B / 250 ms | 4,608,435 |
| `cf3632647a8bb78a00e28150e2f49c7e69ac4b5cea1ea1f02f6790bba428ce21` | the Android companion on a phone, both directions (100.4 s) | testnet |
