/**
 * The QR return path: a message, cut into UR parts that each fit one frame of
 * the 128x64 panel, cycled on a timer.
 *
 * ---------------------------------------------------------------------------
 * Why this is not in ui.c
 *
 * Which frame goes out when, and whether it fits, is arithmetic that can be
 * checked on the host against the real QR encoder. The screen that shows it is
 * a thin wrapper in ui.c. Kept apart, the host can prove that every frame of
 * every mode fits its version before a camera is ever pointed at one.
 *
 * ---------------------------------------------------------------------------
 * The modes
 *
 * The trade is module size against frame count, and only the bench can say
 * which one a phone camera reads off this panel. So all three candidates are
 * here and the user can step between them; the default is the animated,
 * readable one.
 *
 * ---------------------------------------------------------------------------
 * Fit is guaranteed, not hoped for
 *
 * A part's length grows with its sequence number (more digits, a wider CBOR
 * integer), and fountain parts go on forever. So the sequence wraps at
 * QR_OUT_SEQ_WRAP, and the fragment length is chosen at start as the largest
 * one whose part QR_OUT_SEQ_WRAP still fits the mode's alphanumeric capacity.
 * Every frame drawn is therefore no longer than one already measured.
 */

#ifndef LEEK_QR_OUT_H
#define LEEK_QR_OUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Largest message the return path carries: an xpub is ~100 bytes, a .zsig
 * 108 per transparent input (roadmap 10.1) and 69 per shielded spend. */
#define QR_OUT_MAX_MESSAGE 1024

/** Longest part text, which is version 10's alphanumeric capacity. */
#define QR_OUT_MAX_PART 395

/**
 * Where the sequence starts over. Past 255 the CBOR sequence number grows a
 * byte and the part outgrows what was measured; restarting is harmless, since
 * a receiver treats a repeat as redundant.
 */
#define QR_OUT_SEQ_WRAP 255

typedef struct {
    uint8_t     version;     /* 0: the smallest that holds the message whole */
    uint8_t     scale;
    uint16_t    period_ms;
    const char *name;        /* six characters or fewer, for the footer */
    bool        inverted;    /* lit modules on a dark background */
    bool        color;       /* the 240x240 colour panel's, below; never an OLED's */
} QrOutMode;

#define QR_OUT_MODE_COUNT 10
/* v3 at scale 2: the chunkiest modules, 2 panel pixels each. Measured by
   rendering this panel's framebuffer and blurring it: the scale-1 modes need a
   camera to resolve at least 3 of its pixels per 0.17 mm OLED pixel, which a
   1080p webcam only manages within about 7 cm -- inside most webcams' focus
   distance. v3 x2 decodes at 2. The first CAM-board pairing, at v6 x1 on a
   laptop webcam, never saw a code at all. Frames are the price, not failure. */
#define QR_OUT_MODE_DEFAULT 2

/*
 * One still code: the whole message in a single part, at the smallest version
 * (up to 10) that holds it, so nothing animates. On the 64-row OLED that is a
 * 1-pixel-per-module code a camera cannot read, which is why it is not the
 * default there. On a 240x240 panel the code is redrawn at native scale
 * (panel-color.h) - a .zsig reply is version 8 at 4 px per module - and a
 * phone reads it at a glance, so the colour panel starts here and falls back
 * to the animated modes for a message too long for one code.
 */
#define QR_OUT_MODE_STILL 5

/*
 * The colour panel's own animated modes.
 *
 * The OLED modes are sized for 64 rows: v3 x2 exists because nothing bigger
 * than 2 px a module fits there. The 240x240 panel redraws every code from
 * its modules at the largest whole size that keeps a two-module quiet zone
 * (panel-color.c), so there a mode is just a version, and the module size
 * follows from it:
 *
 *     mode     chars/frame   px/module   code     a 108-byte .zsig
 *     v9 4px       335           4       212 px   1 frame (= still)
 *     v6 5px       195           5       205 px   2 frames
 *     v4 6px       114           6       198 px   4 frames
 *     v3 7px        77           7       203 px   9 frames
 *
 * (0.0975 mm a pixel on a 1.3" panel: 4 px is 0.39 mm, 7 px 0.68 mm; the
 * OLED's v3 x2 is 0.34 mm.) Each line is the largest version at its module
 * size, so no mode is beaten on both counts by another.
 *
 * What the colour panel shows first: one still code (QR_OUT_MODE_STILL) if
 * the message fits one at version 9 or below, i.e. 4 px or more; a .zsig for
 * one transparent input is a v8 still. Nothing to reassemble, nothing to
 * miss, and 4 px is already bigger than the OLED default a webcam reads.
 * A longer message (several spends) starts at v6 5px: 2.5 times the OLED
 * default's characters a frame, so a third of its frames, at modules 1.4
 * times as big. A turn (and OK, which has no contrast to step on this
 * panel) goes through still, v6, v4, v3, v9, so a camera that cannot
 * resolve 4-5 px gets 6 and 7 one detent at a time.
 *
 * Appended, so the OLED indices above never move, and skipped by the OLED's
 * cycle (QrOutMode.color). Their OLED scale is 1, which every version up to
 * 10 fits in 64 rows: the 1-bit frame under the colour panel is never shown.
 */
#define QR_OUT_MODE_C_V9  6
#define QR_OUT_MODE_C_V6  7
#define QR_OUT_MODE_C_V4  8
#define QR_OUT_MODE_C_V3  9
#define QR_OUT_COLOR_MODE_DEFAULT QR_OUT_MODE_C_V6
/* The largest still version the colour panel shows first (4 px a module). */
#define QR_OUT_COLOR_STILL_MAX_VERSION 9

const QrOutMode *qr_out_mode_info(uint8_t mode);

/** QR alphanumeric capacity at ECC_LOW, in characters, for versions 1-10. */
uint16_t qr_out_alnum_capacity(uint8_t version);

/**
 * Start showing `cbor` as a `ur:<type>` in `mode`. The message is copied.
 * Returns false if it is too long, the type is invalid, or no fragment length
 * fits the mode - in which case nothing is shown.
 */
bool qr_out_start(const char *type, const uint8_t *cbor, size_t len,
                  uint8_t mode, int64_t now_us);

/** Same message, another mode. Starts again from part 1. */
bool qr_out_set_mode(uint8_t mode, int64_t now_us);

/** Advance if the current frame has been up for its period. True if it did. */
bool qr_out_tick(int64_t now_us);

/** The frame to draw now, uppercased, or NULL when nothing is running. */
const char *qr_out_frame(void);

uint8_t  qr_out_current_mode(void);
uint8_t  qr_out_current_version(void);  /* the mode's, or still's choice */
uint32_t qr_out_seq_len(void);      /* 1 for a static single-part code */
uint32_t qr_out_seq_num(void);

/** Forget the message and zero every buffer. */
void qr_out_stop(void);

#endif /* LEEK_QR_OUT_H */
