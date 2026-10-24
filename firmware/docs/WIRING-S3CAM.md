# Wiring: S3 CAM board + Zcash screen kit

The `esp32s3cam` build (`pio run -e esp32s3cam`) expects this wiring. Pins
are from [BOARD-S3CAM-PINOUT.md](BOARD-S3CAM-PINOUT.md). The camera, the
microSD slot and the octal PSRAM are on the board and need no wiring.

The kit is three parts:

- a **1.5" SH1107 128×128 I2C OLED**;
- an **EC11 rotary encoder**: turn it to move, press it for OK. Which way
  is "down" is switchable on the board under **Settings → Knob direction**;
- **one Cancel button**, K3, on GPIO14.

Every screen, setting and gesture works with just these: turn, press, and
Cancel.

**Power every module from 3V3, never 5V.** Each module has its own pull-up
resistors to its supply pin, and those pull the ESP32-S3's pins up to that
voltage. The S3 is not 5 V tolerant.

## Screen: 1.5" OLED, GME128128-01, SH1107, 128×128, I2C

| Module | Board |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SCL | GPIO21 |
| SDA | GPIO47 |

I2C address 0x3C (set in `src/oled.c`). Some modules are strapped to 0x3D.
If the screen stays dark and the log shows `SH1107 init cmd AE failed`,
that is the first thing to check.

The UI is laid out for 128×64 and is stretched 2× vertically to fill the
panel: every row is drawn twice, so text is tall and the whole glass is
used. QR codes are not stretched; they keep their own full-panel path: a unified address is drawn at 2 pixels per
module and a t-address at 3, so a phone can read them more easily.

If the picture is upside down, build with `-DBOARD_SH1107_FLIP=1`.

## Cancel button: K3

| Button | Board | Function |
|---|---|---|
| one side | GPIO14 | Cancel (BACK) |
| other side | GND | |

Any momentary push button. The pin has the ESP32-S3's internal pull-up, so
a pressed key reads **low**; no resistor is needed. Holding Cancel is also
the lock gesture.

GPIO1 (K1) and GPIO41 (K2) are left unconnected. Their internal pull-ups
keep them reading "released", and the firmware ignores any key that reads
pressed from power-on until it has been seen released, so a stray short
there cannot type or lock.

*Optional, legacy:* the original four-button module (V G 1 2 3 4: V to 3V3,
G to GND, 1 to GPIO1 UP, 2 to GPIO41 DOWN, 3 to GPIO14 Cancel, 4 to GPIO42
OK) still works on the same pins, alongside or instead of the knob. Its keys
must read low when pressed (check with a multimeter: about 3.3 V released,
0 V pressed).

## Rotary encoder: EC11 module, GND + SW DT CLK

| Module | Board | Function |
|---|---|---|
| GND | GND | |
| + | 3V3 | supply for the module's pull-ups |
| CLK | GPIO44, the header pin labelled **RX** | encoder A |
| DT | GPIO43, the header pin labelled **TX**, **through a 1 kΩ resistor** | encoder B |
| SW | GPIO42 | press = OK |

The board's silkscreen labels GPIO43 and GPIO44 **TX** and **RX**, not with
their numbers. They are the top two pins on the right of the header.

Other header pins look free but are not. 4-13 and 15-18 are the camera;
38-40 the microSD; 35-37 the PSRAM; 19-20 USB. 0, 3, 45 and 46 are
strapping pins (a module's pull-up there can stop the board booting or
flashing). 2 is the power LED and 48 the RGB LED.

- Turning the knob is UP and DOWN, one step per detent. Pressing it is OK
  (GPIO42 is the firmware's K4, "accept"; with the optional four-button
  module its key 4 shares the pin, in parallel).
- GPIO43 and GPIO44 are UART0. They are free because the firmware's console
  is USB Serial/JTAG. The ROM still prints its boot banner on GPIO43 (TX) for
  a few milliseconds, though, and if the encoder contact happened to be
  closed then, the pin would be driving into a short. The 1 kΩ resistor
  takes that instead.
- No 1 kΩ to hand? Anything from 220 Ω to 2.2 kΩ works, for example 330,
  470 or 2.2k, or two 2.2k in parallel. Do not use 4.7 kΩ or more: with the
  module's 10 kΩ pull-up the pin would no longer read a clean low. For
  bring-up, DT can also go straight to GPIO43. A standard EC11 has both
  contacts open at every detent, so if the knob rests on a click at power-up
  nothing is shorted. A multimeter from DT to GND with the knob on a click
  should read open. Add the resistor when you have one; the firmware is the
  same either way.
- Clockwise is DOWN: the next menu item, the next page, and in the pickers
  the next option, so a PIN digit goes up. To have it the other
  way round, choose **Settings → Knob direction** on the board (the menu
  item reads `Knob Normal` or `Knob Reversed`): it toggles, applies at once
  and is kept across restarts. It swaps UP and DOWN for the knob only.
- Turning while the knob is held down (press-and-turn) is the scan
  screen's bench toggle for live video to the PC viewer, the one gesture
  that used to need a held UP key.
- If clockwise scrolls up rather than down in Normal, the encoder is wired
  with A and B the other way round: build with `-DBOARD_ENC_REVERSE=1` (or
  swap CLK and DT). The setting flips on top of that, so Normal always
  means "clockwise is DOWN".
- A common EC11 gives one quadrature cycle per detent (20 detents, 20
  pulses). If yours needs two clicks per step, it is the 30/15 kind: build
  with `-DENCODER_HALF_STEP`.

## Camera

OV5640 in the board's camera connector, the same module and connector as
LeekWallet. Nothing changes.

## Summary

```
GPIO47 SDA ── OLED SDA          GPIO14 ── Cancel button (K3) ── GND
GPIO21 SCL ── OLED SCL          GPIO42 ── ENC SW (press = OK)
RX(GPIO44) ── ENC CLK           GPIO1, GPIO41: not connected (K1, K2)
TX(GPIO43) ─1k─ ENC DT
3V3 ── OLED VCC, ENC +          GND ── OLED GND, ENC GND, Cancel button
GPIO38/39/40 microSD (on board)  GPIO4-18 camera (on board)
```

Not checked on hardware yet. The first boot is the bring-up: screen
orientation, the I2C address, button polarity and encoder direction are the
four things that may need one of the build flags above.
