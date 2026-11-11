/**
 * The Scan screen's aiming aid: a camera frame reduced to the panel.
 *
 * Split out from camera.c because it is arithmetic over a byte buffer and
 * nothing else - no sensor, no driver, no ESP-IDF - so the host suite can
 * check that a bright rectangle in the frame lands in the right place on the
 * panel. That is the whole correctness question: a preview that mirrors or
 * rotates the view makes aiming worse than no preview at all, and only a test
 * catches it before a user does.
 *
 * It is deliberately incapable of showing anything but raw camera pixels.
 * Nothing decoded ever reaches this file, so the preview cannot become a way
 * for whoever controls the QR code to put text of their choosing on the
 * trusted display.
 */

#ifndef LEEK_VIEWFINDER_H
#define LEEK_VIEWFINDER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The preview occupies the top seven pages; the eighth is left for the status
 * line, which is the only row on this screen the firmware writes words to.
 */
#define VIEWFINDER_W        128
#define VIEWFINDER_H        56
#define VIEWFINDER_BYTES    (VIEWFINDER_W * VIEWFINDER_H / 8)

/**
 * Reduce a grayscale frame to a 1-bit SSD1306 page bitmap.
 *
 * `out` is VIEWFINDER_BYTES laid out as the panel lays out its framebuffer:
 * one byte per 8-row column, page 0 first, bit 0 the topmost row. A pixel is
 * lit when the frame is brighter there than the midpoint of that frame's own
 * darkest and brightest sampled pixel - per frame, because any fixed threshold
 * is wrong under the next light source.
 *
 * Nearest-neighbour, no interpolation: 320/128 is 2.5 and 240/56 is 4.3, and
 * the user needs to see where the bright rectangle sits, not a photograph.
 */
void viewfinder_render(const uint8_t *frame, uint16_t fw, uint16_t fh,
                       uint8_t *out);

/* ------------------------------------------------------------------------
 * The colour kit's viewfinder: grayscale, at the panel's own resolution.
 *
 * The 1-bit bitmap above is right for a 128x64 OLED and wrong for a 240x240
 * TFT: stretched 1.875x it is a few thick bars, "only lines", and a person
 * cannot tell a QR from a window. The TFT can show 64 grays, so it gets a
 * picture.
 *
 * 200x150, the camera's 4:3, drawn 1:1 on the panel (no resampling there)
 * across its top five text rows (5 x 30 = 150 panel rows), which leaves three
 * rows below it for the exposure, the counters and the status line. 30,000
 * bytes, one byte a pixel, in PSRAM on the board: preview pixels are not
 * secret, and internal RAM is not there to spare. Sending it costs nothing
 * extra on the wire: the colour driver always flushes the whole 240x240
 * frame, 115 KB in bands, about 23 ms at 40 MHz, of which the preview is
 * 60 KB (~12 ms).
 *
 * Bigger was weighed and refused: 240x180 would leave two rows for the
 * status, and the feedback rows are half of what was missing. 160x120 (an
 * exact half of QVGA) would leave four rows and a picture a third smaller,
 * for no row the screen needs.
 */
#define VIEWFINDER_GRAY_W      200
#define VIEWFINDER_GRAY_H      150
#define VIEWFINDER_GRAY_BYTES  (VIEWFINDER_GRAY_W * VIEWFINDER_GRAY_H)

/*
 * The smallest raw range the auto-level stretches to full scale: a gain of
 * 255/64, about 4x, at most. Without a cap a frame with no contrast (a lens
 * cap, a white wall) would be amplified sensor noise; with it, such a frame
 * is a flat gray at its own level, and a darker exposure still looks darker,
 * so turning the knob shows on the picture and not only on the bar.
 */
#define VIEWFINDER_MIN_RANGE   64

/*
 * What a frame looked like, measured on the raw (box-averaged, not yet
 * stretched) preview pixels. Numbers only: nothing here can say anything
 * but how bright, how contrasty and how still the view is.
 */
typedef struct {
    uint8_t lo, hi;        /* darkest and brightest preview pixel, raw */
    uint8_t mean;          /* average raw level */
    uint8_t clipped_pct;   /* pixels at or above 250, percent */
    uint8_t bright_pct;    /* pixels above the (lo + hi) / 2 midpoint, percent */
    uint8_t motion;        /* mean |change| since the previous preview, raw levels;
                              0 when there was no previous preview */
    /* Kept from one call to the next: the stretch applied to `out`, so the
       next frame can be compared with it in raw terms. Zero the struct to
       start; `primed` says `out` holds a preview made with these. */
    bool    primed;
    uint8_t level_lo;
    uint8_t level_range;
} ViewfinderStats;

/**
 * Reduce a grayscale frame to VIEWFINDER_GRAY_W x VIEWFINDER_GRAY_H, one byte
 * a pixel, row-major, top-left first, the same way up as the frame (no
 * mirroring: the camera's orientation bits are what make it right).
 *
 * Each output pixel is the average of the frame pixels under it (a box
 * filter, so QR modules two camera pixels wide average to gray instead of
 * aliasing into stripes). The image keeps the frame's aspect ratio and is
 * centred; any bars are black. Then auto-levelled: the frame's own darkest
 * and brightest pixel are stretched to 0 and 255, the range never narrower
 * than VIEWFINDER_MIN_RANGE.
 *
 * `st` is in/out (see ViewfinderStats). `out` must hold
 * VIEWFINDER_GRAY_BYTES; it is read as well as written, for `motion`.
 *
 * `true_exposure` skips the stretch: the raw averages as the decoder sees
 * them, so a person tuning exposure can judge it from the panel (Settings >
 * Preview). The statistics are the same either way.
 */
/**
 * Turn the grayscale viewfinder by `quarter_turns_ccw` x 90 degrees counter-
 * clockwise (0..3), for a camera mounted at an angle to the panel. Only the
 * preview turns: quirc reads a code in any orientation, and a turn is never a
 * mirror. Default 0.
 */
void viewfinder_set_rotation(uint8_t quarter_turns_ccw);

void viewfinder_render_gray(const uint8_t *frame, uint16_t fw, uint16_t fh,
                            uint8_t *out, ViewfinderStats *st,
                            bool true_exposure);

/**
 * Advice for the person aiming, from the frame statistics and whether quirc
 * located any code since the last preview: "Too dark", "Too bright",
 * "Hold steady", "Move closer", or NULL when there is nothing to say. Always
 * one of those fixed strings.
 */
const char *viewfinder_hint(const ViewfinderStats *st, bool located);

/* The feedback rows, 21 characters at most, from numbers only. */
#define VIEWFINDER_LINE 22

/** "Light <==O---> -2 47%": the exposure bias (-5..0) on a bar, and the view's
 *  mean level. */
void viewfinder_exposure_line(int8_t bias, uint8_t mean, char out[VIEWFINDER_LINE]);

/** "QVGA 12fps s:3 r:1": capture size, frames a second, codes located and
 *  read. Unknown fps (0xFFFF) prints as "--". */
void viewfinder_stats_line(uint16_t frame_w, uint16_t fps, uint32_t seen,
                           uint32_t read, char out[VIEWFINDER_LINE]);

/** "[####------] 12 left": parts of an animated UR assembled, of `total`. */
void viewfinder_progress_line(uint32_t total, uint32_t remaining,
                              char out[VIEWFINDER_LINE]);

#endif /* LEEK_VIEWFINDER_H */
