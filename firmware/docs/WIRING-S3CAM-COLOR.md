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
| CS | GND | the panel is the only device on the bus; the driver runs without a CS pin |
| RES | 3V3 through 10 kΩ | the driver resets the panel in software (SWRESET) |
| BLK | 3V3 | backlight always on |
| TRA / TRB | GPIO44 / GPIO43 | encoder A / B, as on the knob kit; **keep the 1 kΩ resistor in series on GPIO43** |
| PSH (OK) | GPIO42 | the knob's push, as on the knob kit |
| K0 (Cancel) | GPIO14 | as on the knob kit |
| OPTIGA SDA / SCL | GPIO47 / GPIO21 | the I2C bus, now free of the OLED; for the secure element |

No strapping pin (0, 3, 45, 46) is used. GPIO43 is UART0 TX, where the ROM
prints its boot banner; the 1 kΩ resistor is what protects it if the encoder
contact happens to be closed at that moment (same reasoning as the knob kit).

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
| Nothing at all, backlight on | check SCL/SDA are not swapped and DC is on GPIO2; then try `-DST7789_SPI_MODE=0` |
| Speckles or random pixels | long jumper wires: `-DST7789_SPI_HZ=20000000` |
| Knob runs the wrong way | **Settings → Knob direction** on the device, or `-DBOARD_ENC_REVERSE=1` |

The boot log names the panel's settings in one line
(`st7789: 240x240 at (0,0), MADCTL 00, inverted, 40 MHz, SPI mode 3`).

## Seeing the screens without a board

```
cd firmware/sim && make screens     # -> build/screens/*.png
```

renders the real UI through the real colour renderer, next to the SH1107's
view of the same frame.
