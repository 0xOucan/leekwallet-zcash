/**
 * The colour panel bench: a bring-up test for the ST7789 module over its
 * wires alone.
 *
 * Compiled only under LEEK_PANEL_BENCH, an environment of its own
 * (`pio run -e esp32s3cam-panel-bench`), like the QR and SE benches: the
 * wallet never starts, the vault is never opened, and nothing holds a seed.
 * It drives SCL, SDA and DC by hand (bit-banged, no SPI peripheral), so a
 * picture here proves the wiring and the panel independently of st7789.c.
 * It cycles the four SPI clock modes and fills the screen red, green, blue
 * in each, logging every step over USB serial. It also drives GPIO48 as an
 * active-low CS around every command, so moving the module's CS wire from
 * GND to pin 48 can be tested without a reflash.
 */

#ifndef LEEK_PANEL_BENCH_H
#define LEEK_PANEL_BENCH_H

#ifndef LEEK_PANEL_BENCH
#  define LEEK_PANEL_BENCH 0
#endif

#if LEEK_PANEL_BENCH
/** Spawn the bench task. Call it and nothing else. */
void panel_bench_start(void);
#endif

#endif /* LEEK_PANEL_BENCH_H */
