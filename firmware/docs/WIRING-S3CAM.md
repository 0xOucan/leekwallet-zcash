# Wiring: S3 CAM board + Zcash screen kit

The `esp32s3cam` build (`pio run -e esp32s3cam`) expects this wiring. Pins
are from [BOARD-S3CAM-PINOUT.md](BOARD-S3CAM-PINOUT.md). The camera, the
microSD slot and the octal PSRAM are on the board and need no wiring.

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

The UI is laid out for 128×64 and appears in the middle of the panel. QR
codes fill the whole panel: a unified address is drawn at 2 pixels per
module and a t-address at 3, so a phone can read them more easily.

If the picture is upside down, build with `-DBOARD_SH1107_FLIP=1`.

## Four-button module: V G 1 2 3 4

| Module | Board | Function |
|---|---|---|
| V | 3V3 | |
| G | GND | |
| 1 | GPIO1 | UP |
| 2 | GPIO41 | DOWN |
| 3 | GPIO14 | BACK |
| 4 | GPIO42 | OK |

The firmware expects a pressed key to read **low** (pulled up, switch to
GND). Check with a multimeter before connecting: with V on 3V3 and G on GND,
a key output should read about 3.3 V released and 0 V pressed. If it reads
0 V released and 3.3 V pressed, the module is active-high. Say so, and the
input polarity becomes a board setting.

## Rotary encoder: EC11 module, GND + SW DT CLK

| Module | Board | Function |
|---|---|---|
| GND | GND | |
| + | 3V3 | supply for the module's pull-ups |
| CLK | GPIO44 | encoder A |
| DT | GPIO43 **through a 1 kΩ resistor** | encoder B |
| SW | GPIO42 (same pin as button 4) | press = OK |

- Turning the knob is UP and DOWN, one step per detent. Pressing it is OK.
  It is wired in parallel with button 4, which works because both pull the
  line to GND.
- GPIO43 and GPIO44 are UART0. They are free because the firmware's console
  is USB Serial/JTAG. The ROM still prints its boot banner on GPIO43 (TX) for
  a few milliseconds, though, and if the encoder contact happened to be
  closed then, the pin would be driving into a short. The 1 kΩ resistor
  takes that instead.
- If clockwise scrolls up rather than down, build with
  `-DBOARD_ENC_REVERSE=1`.
- A common EC11 gives one quadrature cycle per detent (20 detents, 20
  pulses). If yours needs two clicks per step, it is the 30/15 kind: build
  with `-DENCODER_HALF_STEP`.

## Camera

OV5640 in the board's camera connector, the same module and connector as
LeekWallet. Nothing changes.

## Summary

```
GPIO47 SDA ── OLED SDA          GPIO1  ── BTN 1 (UP)
GPIO21 SCL ── OLED SCL          GPIO41 ── BTN 2 (DOWN)
GPIO44     ── ENC CLK           GPIO14 ── BTN 3 (BACK)
GPIO43 ─1k─ ENC DT              GPIO42 ── BTN 4 (OK) + ENC SW
3V3 ── OLED VCC, BTN V, ENC +   GND ── OLED GND, BTN G, ENC GND
GPIO38/39/40 microSD (on board)  GPIO4-18 camera (on board)
```

Not checked on hardware yet. The first boot is the bring-up: screen
orientation, the I2C address, button polarity and encoder direction are the
four things that may need one of the build flags above.
