/**
 * Which board this firmware is being built for, and what is fitted to it.
 *
 * ---------------------------------------------------------------------------
 * Why this exists
 *
 * The pin numbers were spread across the files that used them, which was fine
 * while there was one board and actively dangerous the moment there were two.
 * The reference build drives I2C on GPIO 8 and 9; on a Firefly Pixie those are
 * **Button 2 and the WS2812B data line**, and GPIO9 is additionally the
 * ESP32-C3's boot strapping pin - held low at reset it enters download mode.
 * Flashing the unmodified reference build onto a Pixie would therefore drive an
 * I2C clock down the LED string and toggle the strapping pin, which damages
 * nothing but is exactly the sort of thing nobody notices until a board behaves
 * strangely at boot.
 *
 * One header, selected by the build, so a pin is chosen once and the compiler
 * catches the case nobody thought about.
 *
 * ---------------------------------------------------------------------------
 * Boards and components are two different questions
 *
 * A board is a set of GPIOs: which ones exist, which ones the camera or the
 * flash already owns, which ones are safe at boot. A component is a part
 * somebody fitted to those GPIOs: a panel, a knob, a secure element. The first
 * version of this file answered both at once, one `#if` branch per board with
 * the panel and the buttons baked in, which was fine for three boards with one
 * kit each and would have meant a new branch - and a new copy of every pin -
 * for each new screen on an old board.
 *
 * So, now:
 *
 *   - boards/<board>.h is the pin map: GPIOs, the board's own peripherals
 *     (camera, SD slot, LEDs), and which components it can carry and on which
 *     pins. Adding a board is one short file.
 *   - This file picks the board, picks the components (the board's usual kit
 *     unless the build names another), refuses a combination the board cannot
 *     carry, and derives the flags the rest of the firmware has always read.
 *   - A component is one driver plus one value below. Adding a panel does not
 *     touch any board that does not carry it.
 *   - A build is a named combination of the two in platformio.ini, so it is
 *     reproducible like every other environment.
 *
 * ---------------------------------------------------------------------------
 * Selecting a board
 *
 * There are three boards and only two targets. The reference board and the CAM
 * board are both `esp32s3`, so the target alone stopped being enough to tell
 * them apart the moment the CAM board existed, and a header that guesses wrong
 * here drives peripherals down pins that belong to something else. That is the
 * mistake this file was written to prevent, so the board is named explicitly
 * by the build.
 *
 * `platformio.ini` passes exactly one of `-DLEEK_BOARD_S3`,
 * `-DLEEK_BOARD_PIXIE` or `-DLEEK_BOARD_S3CAM`. A build that passes none falls
 * back to the old behaviour - C3 means Pixie, anything else means the
 * reference board - so an out-of-tree build that predates this change keeps
 * working and produces the same image. A build that passes two is a mistake
 * and is refused below rather than silently resolved.
 */

#ifndef LEEK_BOARD_H
#define LEEK_BOARD_H

#include "driver/gpio.h"

#if !defined(LEEK_BOARD_S3) && !defined(LEEK_BOARD_PIXIE) && \
    !defined(LEEK_BOARD_S3CAM)
#  if defined(CONFIG_IDF_TARGET_ESP32C3)
#    define LEEK_BOARD_PIXIE 1
#  else
#    define LEEK_BOARD_S3 1
#  endif
#endif

/*
 * Normalise to 0/1 before checking, so the guard and the dispatch below ask the
 * same question. They did not, in the first version of this: the guard tested
 * `defined(...)` while the dispatch tested the value, so `-DLEEK_BOARD_PIXIE=0`
 * satisfied the guard, matched no branch, and fell through to whichever board
 * happened to be last. Found in review before it ever ran.
 */
#ifndef LEEK_BOARD_S3
#  define LEEK_BOARD_S3 0
#endif
#ifndef LEEK_BOARD_PIXIE
#  define LEEK_BOARD_PIXIE 0
#endif
#ifndef LEEK_BOARD_S3CAM
#  define LEEK_BOARD_S3CAM 0
#endif

#if (LEEK_BOARD_S3 + LEEK_BOARD_PIXIE + LEEK_BOARD_S3CAM) != 1
#  error "Define exactly one of LEEK_BOARD_S3, LEEK_BOARD_PIXIE, LEEK_BOARD_S3CAM as 1"
#endif

/*
 * ---------------------------------------------------------------------------
 * Components
 *
 * Values, not flags, so a build names exactly one of each and cannot name
 * two: `-DLEEK_DISPLAY=LEEK_DISPLAY_SSD1306`. Left out, each falls back to the
 * board's usual kit (BOARD_DEFAULT_*), which is how every environment that
 * predates this list still builds the image it always built.
 */

/* Display. One driver each: oled.c (both I2C OLEDs), oled-st7789.c. */
#define LEEK_DISPLAY_SSD1306    1   /* 128x64 OLED, I2C */
#define LEEK_DISPLAY_SH1107     2   /* 128x128 OLED, I2C */
#define LEEK_DISPLAY_ST7789     3   /* 240x240 colour TFT, SPI */

/*
 * Input. Both end as the same four events (button.h): UP, DOWN, CANCEL, OK.
 * The knob kit is an EC11 whose turn is UP/DOWN and whose push is OK, plus
 * one Cancel key; hints.h words the footers for whichever kit is fitted. New
 * builds use the knob. The four-button module is kept for the boards that
 * shipped with it, not offered to new ones.
 */
#define LEEK_INPUT_BUTTONS4     1
#define LEEK_INPUT_KNOB         2

/*
 * Secure element. A hook, not a driver: main.c asks se_probe() (se.h) once at
 * boot. LEEK_SE_NONE links the stub in se-none.c, which reports nothing
 * fitted; a component that drives a real part supplies se_probe() itself and
 * its build names it here.
 */
#define LEEK_SE_NONE            0
#define LEEK_SE_OPTIGA          1   /* Infineon OPTIGA Trust M, on the I2C bus */

/*
 * The board file, first pass: its name, its usual kit, and which components
 * it has pins for (BOARD_DEFAULT_* / BOARD_SUPPORTS_*). The pins come in the
 * second pass below, once the components are chosen, because which GPIO is
 * a key and which is a panel's clock can depend on what is fitted.
 */
#if LEEK_BOARD_PIXIE
#  define LEEK_BOARD_FILE "boards/pixie.h"
#elif LEEK_BOARD_S3
#  define LEEK_BOARD_FILE "boards/s3.h"
#elif LEEK_BOARD_S3CAM
#  define LEEK_BOARD_FILE "boards/s3cam.h"
#else
#  error "No board matched; see the LEEK_BOARD_* guard above"
#endif
#include LEEK_BOARD_FILE

/*
 * The panel, named by the build or the board. `-DBOARD_PANEL_SH1107=0` is the
 * older spelling of "the 128x64 SSD1306 on the CAM board" and still means it.
 */
#ifndef LEEK_DISPLAY
#  if defined(BOARD_PANEL_SH1107)
#    define LEEK_DISPLAY  (BOARD_PANEL_SH1107 ? LEEK_DISPLAY_SH1107 : LEEK_DISPLAY_SSD1306)
#  else
#    define LEEK_DISPLAY  BOARD_DEFAULT_DISPLAY
#  endif
#endif
#ifndef LEEK_INPUT
#  define LEEK_INPUT      BOARD_DEFAULT_INPUT
#endif
#ifndef LEEK_SE
#  define LEEK_SE         LEEK_SE_NONE
#endif

#if !BOARD_SUPPORTS_DISPLAY(LEEK_DISPLAY)
#  error "This board has no pins for the selected LEEK_DISPLAY; see boards/<board>.h"
#endif
#if !BOARD_SUPPORTS_INPUT(LEEK_INPUT)
#  error "This board has no pins for the selected LEEK_INPUT; see boards/<board>.h"
#endif

/* The board file, second pass: the pins, for the components just chosen. */
#define LEEK_BOARD_PINS 1
#include LEEK_BOARD_FILE

/*
 * ---------------------------------------------------------------------------
 * Derived flags
 *
 * The names the firmware has always tested. Every build defines every one, so
 * a `#if LEEK_HAS_ENCODER` is a compile-time zero rather than an undefined
 * identifier, and a typo in a flag name fails the build instead of quietly
 * evaluating false. They are about what is FITTED; whether a feature is on at
 * runtime is a separate question the firmware answers elsewhere. The radios are
 * the clearest case: the CAM board has both, and both are off until the user
 * deliberately turns one on.
 */
#define BOARD_HAS_I2C_PANEL     (LEEK_DISPLAY == LEEK_DISPLAY_SSD1306 || \
                                 LEEK_DISPLAY == LEEK_DISPLAY_SH1107)
#define BOARD_HAS_SPI_PANEL     (LEEK_DISPLAY == LEEK_DISPLAY_ST7789)
#ifndef BOARD_PANEL_SH1107
#  define BOARD_PANEL_SH1107    (LEEK_DISPLAY == LEEK_DISPLAY_SH1107)
#endif
#if BOARD_PANEL_SH1107 != (LEEK_DISPLAY == LEEK_DISPLAY_SH1107)
#  error "BOARD_PANEL_SH1107 disagrees with LEEK_DISPLAY; name the panel once"
#endif
#define LEEK_HAS_ENCODER        (LEEK_INPUT == LEEK_INPUT_KNOB)
#define LEEK_HAS_SE             (LEEK_SE != LEEK_SE_NONE)

/* Component options, each with the value that matches the shipped hardware. */
#ifndef BOARD_SH1107_FLIP
#  define BOARD_SH1107_FLIP     0   /* 1 if the picture is upside down */
#endif
#ifndef BOARD_ENC_REVERSE
#  define BOARD_ENC_REVERSE     0   /* 1 if clockwise moves up instead of down */
#endif

#endif /* LEEK_BOARD_H */
