# QR signing bench (experimental)

Time for the board to read a `ur:leek-pczt` loop, per speed preset. QR
signing is experimental; USB and Bluetooth stay the everyday links.

How to measure: build a testnet send (`zecz send ... --out tx.pczt`, or the
app's review), then `zecz qr-show --in tx.pczt --sweep` (or pick the preset
under *Speed* in the app's *Sign with QR*). On the board choose *Scan*; time
from the first frame to the board saying the scan is complete. Cancel on the
board and scan again before the next preset: a new fragment size is a new
message to its decoder. Note the board's camera settings (resolution, zoom,
exposure) and the screen's brightness.

Rule of thumb from the Ethereum LeekWallet's benches on this board: each
frame must stay up at least twice the board camera's capture interval (at
~7 fps, about 300 ms or more). The board scans at about 7 fps in QVGA today;
the faster presets are for after its camera tuning (a 2x/3x zoom).

View size: ______ B (the review shows it). Firmware: ______ Camera: ______

| Preset (fragment / frame) | Frames a pass | Desktop webcam screen: time to complete | Phone screen: time to complete | Notes |
|---|---|---|---|---|
| 90 B / 500 ms (Safe, default) | 16 | | | |
| 90 B / 350 ms | 16 | | | |
| 120 B / 400 ms (Balanced) | 12 | | | |
| 150 B / 350 ms | 10 | | | |
| 150 B / 250 ms (Fast) | 10 | | | |
| 200 B / 300 ms | 7 | | | |
| 50 B / 500 ms (the Zodl fork's) | 28 | | | |

Frames a pass are for a 1,373-byte compact view (`vectors/pczt/iw_send_nu7_compact.pczt`);
`zecz qr-show --sweep` prints the count for the real one. The return path
(the board's `ur:leek-zsig`, about 110 bytes) is one still code on the colour
panel and a few frames on the OLED.
