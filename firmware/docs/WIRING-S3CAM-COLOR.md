# Wiring: S3 CAM board + colour screen kit

The `esp32s3cam-color` build (`pio run -e esp32s3cam-color`) expects this
wiring. It is the same board as [WIRING-S3CAM.md](WIRING-S3CAM.md) with a
different screen: a square **240×240 colour SPI TFT (ST7789)** module with a
**rotary encoder and one button built in**. Pins are from
[BOARD-S3CAM-PINOUT.md](BOARD-S3CAM-PINOUT.md); the camera, the microSD slot
and the octal PSRAM are on the board and need no wiring.

The module's header reads `GND VCC SCL SDA RES DC CS BLK TRA TRB PSH K0`.
Despite the names, SCL and SDA are **SPI**, not I2C: SCL is the clock and SDA
is MOSI (data into the panel). TRA/TRB are the encoder's A/B, PSH its push
switch (OK), and K0 the extra button (Cancel).

Controls are the knob kit's: turn to move, press for OK, K0 for Cancel. The
footers say so the same way (`X:BACK  TURN  OK:SEL`). There is no
four-button option on this kit: the panel's SPI uses K1's and K2's pins.

**Power the module from 3V3, never 5V.** Its pull-ups go to its own supply,
and the ESP32-S3 is not 5 V tolerant.

## The table

| Module | ESP32-S3 CAM | Notes |
|---|---|---|
| VCC | 3V3 | 3.3 V only |
| GND | GND | |
| SCL (SPI clock) | GPIO1 | was K1, unused on the knob kit |
| SDA (SPI MOSI) | GPIO41 | was K2, unused on the knob kit |
| DC | GPIO2 | also drives the onboard LED, which may flicker with traffic; fine as an output |
| CS | GPIO48 | **required**: with CS on GND the panel stays black (it never re-syncs to byte boundaries). GPIO48 is the onboard RGB LED's pin, which the firmware does not use |
| RES | 3V3 through 10 kΩ | the driver resets the panel in software (SWRESET) |
| BLK | 3V3 | backlight always on |
| TRA / TRB | GPIO3 / GPIO46 | encoder A / B, wired straight (no resistor). **Not** RX/TX (44/43): the module's encoder could not pull those low |
| PSH (OK) | GPIO42 | the knob's push, as on the knob kit |
| K0 (Cancel) | GPIO14 | as on the knob kit |
| OPTIGA SDA / SCL | GPIO47 / GPIO21 | the I2C bus, now free of the OLED; for the secure element |

The encoder sits on two strapping pins, the only free pins left, and both
are harmless here: GPIO3 picks the JTAG source only with `EFUSE_STRAP_JTAG_SEL`
burned (this project burns no eFuses), and GPIO46 matters only in download
mode, which also needs BOOT (GPIO0) held low. On the first colour kit RX and
TX (GPIO44/43) never read low with the module's encoder on them, since both
also go to the board's USB-serial chip, so UART0 is left unused. Don't move
anything to GPIO45: a pull-up there changes the flash voltage at boot.

If the encoder module has no pull-ups of its own, the firmware's internal
ones are enough for short wires.

## What the screen shows

The UI is the same eight rows of 21 characters as on the OLEDs, spread over
the whole panel, with every character redrawn crisply at exactly 2×3 pixels
(`src/panel-color.c`). Titles are leek green, footers dim, the selected
item sits on a green bar, warnings (`!TRANSPARENT: public`, *Wrong network*)
are amber and destructive or failed things red. The words are the same as on
a monochrome board, so nothing depends on seeing the colour.

QR codes are redrawn square at native resolution with a four-module quiet
zone. A `.zsig` reply (~108 bytes) is **one still QR** (version 8, 4 px per
module) instead of an animation; anything too long for one code falls back
to the animated modes, and turning the knob still steps through them.

## Bring-up: when the picture is wrong

Every one of these is a build flag in `platformio.ini`
(`[env:esp32s3cam-color]`, commented out there); nothing in `src/` needs to
change.

| Symptom | Fix |
|---|---|
| Black is white, colours are photo-negative | `-DST7789_INVERT=0` (default 1, right for IPS modules) |
| Red and blue swapped (the green is fine, warnings look blue) | `-DST7789_BGR=1` |
| Upside down or sideways | `-DST7789_ROTATION=1`, `2` or `3` (a quarter turn each) |
| Picture shifted, a band of noise on one edge | the RAM offset: rotations 2 and 3 already add 80; otherwise `-DST7789_X_OFFSET=` / `-DST7789_Y_OFFSET=` |
| Nothing at all, backlight on | CS must be on GPIO48, not GND; check SCL/SDA are not swapped and DC is on GPIO2. Then run the panel bench (below) |
| Speckles or random pixels | long jumper wires: `-DST7789_SPI_HZ=20000000` |
| Knob runs the wrong way | **Settings → Knob direction** on the device, or `-DBOARD_ENC_REVERSE=1` |

The boot log names the panel's settings in one line
(`st7789: 240x240 at (0,0), MADCTL 00, inverted, 40 MHz, SPI mode 3`).

## The panel bench

`pio run -e esp32s3cam-panel-bench -t upload` flashes a test image with no
wallet and no vault access. It drives SCL, SDA, DC and CS by hand (no SPI
peripheral), cycles SPI modes 3, 0, 2 and 1, and fills the screen red, green
and blue in each, logging each step on USB serial. Colour on the screen proves
the wiring and the panel; flash `esp32s3cam-color` again afterwards. This is
how the CS requirement above was found on the first kit (2026-11-10).

## Seeing the screens without a board

```
cd firmware/sim && make screens     # -> build/screens/*.png
```

renders the real UI through the real colour renderer, next to the SH1107's
view of the same frame.
