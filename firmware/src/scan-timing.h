/**
 * Where a scan's time goes, as numbers read back from the board.
 *
 * The camera tuning (docs/JOURNAL.md, "Camera tuning") changes a dozen things
 * that each claim to make scanning faster. Without a split of the frame time
 * none of those claims can be checked: "frames/s went up" does not say whether
 * the sensor, quirc or the panel moved. So every frame on the Scan screen (and
 * in the esp32s3cam-bench image) adds its stage times here, and camera.c
 * prints one line a second over USB serial:
 *
 *   I camera: scan 9.8 fps | wait 41.2/88 copy 1.1/2 ident 38.0/61
 *             dec 3.2/19 prev 1.9/3 draw 24.0/31 ms | s:12 r:4 | ...
 *
 * each stage as mean/max milliseconds over that second. `wait` is the time
 * blocked in esp_camera_fb_get(): large means the sensor is the limit (its
 * frame rate, or the rolling readout); near zero means a frame was already
 * waiting and the CPU is the limit. `ident` is quirc_end() (threshold +
 * finder search), `dec` is extract + decode of every located code, `prev` the
 * viewfinder downsample, `draw` the UI's render + panel flush on the Scan
 * screen.
 *
 * And the number a user actually feels: the time from the first part of an
 * animated UR to the complete message, with how many parts it took against
 * the message's seqLen (the fountain overhead), logged once per message.
 *
 * Pure arithmetic and formatting, no ESP-IDF: host-tested in
 * sim/test_scan_timing.c. Locking, where two tasks add to one ScanTiming, is
 * the caller's (camera.c).
 */
#ifndef LEEK_SCAN_TIMING_H
#define LEEK_SCAN_TIMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SCAN_STAGE_WAIT = 0,    /* blocked in esp_camera_fb_get() */
    SCAN_STAGE_COPY,        /* frame into quirc's image plane */
    SCAN_STAGE_IDENTIFY,    /* quirc_end() */
    SCAN_STAGE_DECODE,      /* quirc_extract() + quirc_decode(), all codes */
    SCAN_STAGE_PREVIEW,     /* viewfinder render */
    SCAN_STAGE_DRAW,        /* UI render + panel flush (Scan screen) */
    SCAN_STAGE_COUNT
} scan_stage_t;

typedef struct {
    uint32_t n[SCAN_STAGE_COUNT];
    uint64_t sum_us[SCAN_STAGE_COUNT];
    uint32_t max_us[SCAN_STAGE_COUNT];
} ScanTiming;

void scan_timing_reset(ScanTiming *t);
void scan_timing_add(ScanTiming *t, scan_stage_t stage, uint32_t us);

/** Mean of a stage in microseconds; 0 with no samples. */
uint32_t scan_timing_mean_us(const ScanTiming *t, scan_stage_t stage);

/**
 * "wait 41.2/88 copy 1.1/2 ident 38.0/61 dec 3.2/19 prev 1.9/3 draw 24.0/31"
 * (mean with one decimal / max, milliseconds). A stage with no samples is
 * "-". Always NUL-terminated; truncated if `size` is short.
 */
size_t scan_timing_format(const ScanTiming *t, char *out, size_t size);

/*
 * Time to a complete animated UR.
 *
 * scan_ur_part() for every part fed to the decoder (new or redundant), with
 * the clock and the camera's frame counter; the first one starts the clock.
 * scan_ur_done() when the message assembled: fills the result and stops.
 * scan_ur_reset() when the screen opens or the session is abandoned.
 */
typedef struct {
    bool     running;
    int64_t  start_us;
    uint32_t start_frames;
    uint32_t parts;        /* parts fed, including redundant */
    uint32_t redundant;
} ScanUrClock;

typedef struct {
    uint32_t ms;           /* first part to complete */
    uint32_t frames;       /* camera frames over that time */
    uint32_t parts;        /* parts fed */
    uint32_t redundant;
    uint32_t seq_len;      /* the message's part count */
} ScanUrResult;

void scan_ur_reset(ScanUrClock *c);
void scan_ur_part(ScanUrClock *c, int64_t now_us, uint32_t frames, bool redundant);
/** False if no part was ever seen (a single-frame UR still counts: 0 ms). */
bool scan_ur_done(ScanUrClock *c, int64_t now_us, uint32_t frames, uint32_t seq_len,
                  ScanUrResult *out);

/** "UR complete: 1234 ms, 31 frames, 14 parts for seqLen 12 (2 redundant, 117%)" */
size_t scan_ur_format(const ScanUrResult *r, char *out, size_t size);

#endif /* LEEK_SCAN_TIMING_H */
