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
-4. With a code in view, the board decodes about 3.5 frames a second. A
frame has to stay up at least twice that long, so do not go below 500 ms
until the camera is faster.

**From the board:** the board can't tell a phone from a webcam, so you pick
the density on the knob (below). A laptop webcam reads the default (v6). A
phone held close can read the denser v9.

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
