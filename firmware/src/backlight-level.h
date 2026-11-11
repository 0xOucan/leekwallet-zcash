/**
 * The colour panel's backlight levels: the one table Settings > Screen steps
 * through, for every screen, QRs included.
 *
 * From 1 % up, the steps an earlier firmware had and nobody asked to change:
 * 1, 2, 3, 5, 7, 10, 15, 20, then tens. Below 1 % is where a webcam needs
 * it: even 1 % was "far too bright" for one reading a QR off the panel
 * (board, 2026-11-10). So below 1 % the table goes as fine and as low as the
 * LEDC allows: 0.7, 0.5, 0.3, 0.2, 0.1, 0.07, 0.05, 0.03, 0.02, 0.01 % and
 * then a single tick, 1/16383 = 0.006 %, the least light the hardware can
 * make short of off.
 *
 * Duties are out of BL_DUTY_MAX: the ESP32-S3's LEDC timers are 14 bits wide
 * (SOC_LEDC_TIMER_BIT_WIDTH), the most resolution there is, at BL_PWM_HZ
 * (oled-st7789.c says why that frequency). Each level carries the label it is
 * shown with, written out rather than computed, because a computed "0.0183%"
 * says nothing a user can repeat back.
 *
 * The table goes all the way down in Settings > Screen, which is where the
 * user picks the level a webcam reads the panel's QRs at. A screen that dark
 * is hard to read in a lit room; it is still lit, so the knob can always
 * bring it back.
 *
 * ---------------------------------------------------------------------------
 * Storage
 *
 * The UI saves the index (NVS bl_idx). The percent an earlier firmware saved
 * (backlight) is read into the nearest level with bl_level_from_pct() and
 * left where it is; so is the separate QR level one saved (qr_bl), which is
 * no longer read. Pure, host-tested in test_ui.
 */
#ifndef LEEK_BACKLIGHT_LEVEL_H
#define LEEK_BACKLIGHT_LEVEL_H

#include <stddef.h>
#include <stdint.h>

/** Full duty of the 14-bit LEDC channel on BLK. */
#define BL_DUTY_BITS    14
#define BL_DUTY_MAX     ((1u << BL_DUTY_BITS) - 1)

/** The PWM frequency. See oled-st7789.c. */
#define BL_PWM_HZ       2000

#define BL_LEVEL_COUNT  27
#define BL_LEVEL_FULL   (BL_LEVEL_COUNT - 1)
/** 1 %: the first of the old steps; below it, the fine ones. */
#define BL_LEVEL_1PCT   11

/** Where the panel starts: full, as the module was on 3V3 before it had a pin. */
#define BL_LEVEL_DEFAULT     BL_LEVEL_FULL

/** The duty at `level`, clamped to the table. */
uint16_t bl_level_duty(uint8_t level);

/** One level up (dir > 0) or down, held at the ends. */
uint8_t bl_level_step(uint8_t level, int dir);

/** The level nearest (in ratio) to an old percent value, 1..100; 0 and
 *  anything above 100 give the default. An old step is matched exactly. */
uint8_t bl_level_from_pct(unsigned pct);

/** "0.006%" .. "0.7%", "1%" .. "100%". `out` takes 8. */
void bl_level_label(uint8_t level, char *out, size_t n);

#endif /* LEEK_BACKLIGHT_LEVEL_H */
