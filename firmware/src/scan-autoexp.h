/**
 * Automatic exposure search for the Scan screen: what to do when the camera
 * keeps FINDING a code and keeps failing to READ it.
 *
 * The bench (docs/zcash/QR-BENCH.md, 2026-11-11): from LCD screens the board
 * reads about 80 % of the codes quirc locates. From an OLED phone at 50-75 %
 * brightness with the bias at -4/-5 it located the code ~100 times and read it
 * once or twice; the phone's anti-flicker (DC dimming) or 100 % brightness
 * fixed most of it. An OLED dims by PWM, the OV5640's rolling shutter turns
 * the flicker into dark horizontal bands, and a band through a code corrupts
 * modules: format and ECC errors on a code that was found. A longer exposure
 * integrates over more PWM periods and the bands flatten out; a different
 * banding step changes how the exposure lines up with them.
 *
 * So this module watches the camera's two counters - codes located and codes
 * decoded, both cumulative since camera_start() - and when, over the last
 * AUTOEXP_WINDOW_MS, at least AUTOEXP_TRIGGER_MIN codes were located and
 * fewer than AUTOEXP_TRIGGER_PERMILLE of them read, it tries a short ordered
 * list of settings (autoexp_setting()): a longer shutter cap, a brighter
 * bias, the other banding frequency. Each is given AUTOEXP_SETTLE_MS for the
 * AEC to converge (not counted) and then judged over AUTOEXP_HOLD_MS by
 * decoded / located. The first to reach AUTOEXP_GOOD_PERMILLE ends the search
 * early; otherwise the best one is kept if it beat the setting that triggered
 * the search by AUTOEXP_MARGIN_PERMILLE, and the search goes back otherwise.
 *
 * It never fights the user: autoexp_user_override() (a knob turn on the Scan
 * screen) stops it for the rest of the scan, and the caller goes back to the
 * user's own setting. The user's bias is never changed: the module only says
 * what EFFECTIVE bias, shutter multiple and banding to apply on top of it, and
 * camera_start() writes the user's setting again on every scan.
 *
 * No PWM detection from the picture itself: a QR code is a pattern of
 * horizontal edges as strong as any band, and without a measured line time a
 * band period in rows says nothing about the PWM frequency. "Located but not
 * read" is the symptom that matters and it costs nothing to count.
 *
 * Pure: no ESP-IDF, no clock of its own; host-tested with scripted counter
 * sequences in sim/test_scan_autoexp.c. camera.c feeds it numbers once a
 * frame and applies its decisions (CAMERA_AUTO_EXPOSURE).
 */
#ifndef LEEK_SCAN_AUTOEXP_H
#define LEEK_SCAN_AUTOEXP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Trigger: a sliding window of this long ... */
#define AUTOEXP_WINDOW_MS         2000
/* ... with at least this many codes located ... */
#define AUTOEXP_TRIGGER_MIN       5
/* ... and fewer than this many in 1000 of them decoded (10 %; an LCD reads
   ~80 %, the bad OLED case ~1 %, and with ~10 located in a window one
   decode either way must not decide it). */
#define AUTOEXP_TRIGGER_PERMILLE  100
/* Each trial: not judged while the AEC converges on the new setting ... */
#define AUTOEXP_SETTLE_MS         700
/* ... then judged over this long ... */
#define AUTOEXP_HOLD_MS           2000
/* ... needing this many located to mean anything; fewer, and the hold is
   extended once, then the trial counts as reading nothing. */
#define AUTOEXP_JUDGE_MIN         4
/* A trial that reads this well ends the search there (40 %). */
#define AUTOEXP_GOOD_PERMILLE     400
/* What a trial has to beat the triggering setting by to be kept (5 points),
   with at least this many decodes in its hold: one lucky read out of ten
   located is 10 % and means nothing. */
#define AUTOEXP_MARGIN_PERMILLE   50
#define AUTOEXP_KEEP_MIN_DECODED  2
/* Searches per scan; after a search that kept nothing, wait this long before
   the next can start. */
#define AUTOEXP_MAX_SEARCHES      2
#define AUTOEXP_COOLDOWN_MS       10000
/* Snapshots of the counters for the sliding window. */
#define AUTOEXP_SAMPLE_MS         250
#define AUTOEXP_RING              16

/* One setting, relative to the user's. Index 0 is the user's own. */
typedef struct {
    int8_t  bias_add;      /* added to the user's bias, then clamped -5..0 */
    uint8_t shutter_mul;   /* x the build's shutter cap (CAMERA_SHUTTER_CAP_LINES) */
    bool    mains_swap;    /* banding at the other mains frequency (60 <-> 50) */
} AutoExpSetting;

#define AUTOEXP_SETTINGS  6
/** Setting 0..AUTOEXP_SETTINGS-1; out of range is setting 0. */
const AutoExpSetting *autoexp_setting(uint8_t index);

typedef enum {
    AUTOEXP_IDLE = 0,    /* watching at the current setting */
    AUTOEXP_SETTLING,    /* a trial setting applied, AEC converging */
    AUTOEXP_JUDGING,     /* a trial setting being scored */
    AUTOEXP_COOLDOWN,    /* a search kept nothing; not watching yet */
    AUTOEXP_DONE,        /* out of searches; holding the current setting */
    AUTOEXP_STOPPED,     /* the user turned the knob: off for this scan */
} autoexp_phase_t;

typedef enum {
    AUTOEXP_EV_NONE = 0,
    AUTOEXP_EV_TRY,      /* apply autoexp_current(): a trial starts */
    AUTOEXP_EV_KEEP,     /* apply autoexp_current(): the best trial is kept */
    AUTOEXP_EV_REVERT,   /* apply autoexp_current(): back to the pre-search setting */
    AUTOEXP_EV_STOP,     /* the user took over: apply the user's setting */
} autoexp_event_t;

typedef struct {
    uint32_t t_ms;
    uint32_t located;
    uint32_t decoded;
} AutoExpSample;

typedef struct {
    autoexp_phase_t phase;
    int8_t   user_bias;
    uint8_t  current;          /* setting applied now */
    uint8_t  origin;           /* setting the search started from */
    uint8_t  next_pos;         /* position in the search order */
    uint8_t  best;             /* best setting so far this search */
    uint16_t best_score;       /* its decoded/located, permille */
    uint16_t origin_score;     /* the triggering window's, permille */
    uint32_t best_decoded;     /* decodes in the best trial's hold */
    uint8_t  searches;
    uint8_t  extended;         /* the judging hold was extended once */
    uint8_t  tried_mask;       /* settings tried this search (bit per index) */
    uint32_t phase_ms;         /* when the phase started */
    uint32_t mark_located, mark_decoded;   /* counters at the judging start */
    /* The last judgement or trigger window, for the log. */
    uint32_t last_located, last_decoded;
    uint16_t last_score;
    AutoExpSample ring[AUTOEXP_RING];
    uint8_t  ring_n, ring_head;
} AutoExp;

/** What to apply: the effective bias and the setting's other fields. */
typedef struct {
    uint8_t index;          /* 0 = the user's own setting */
    int8_t  bias;           /* effective, -5..0 */
    uint8_t shutter_mul;
    bool    mains_swap;
} AutoExpApply;

/** At camera_start(): the user's bias, setting 0, watching. */
void autoexp_reset(AutoExp *a, uint32_t now_ms, int8_t user_bias);

/**
 * Once a frame (or more often): the cumulative located and decoded counters.
 * Returns an event; anything but AUTOEXP_EV_NONE means the setting changed
 * and the caller applies autoexp_current().
 */
autoexp_event_t autoexp_tick(AutoExp *a, uint32_t now_ms, uint32_t located,
                             uint32_t decoded);

/**
 * The user turned the knob: the search stops for this scan and the user's new
 * bias applies with setting 0. Returns AUTOEXP_EV_STOP the first time (the
 * caller restores shutter and banding if autoexp had moved them), NONE after.
 */
autoexp_event_t autoexp_user_override(AutoExp *a, int8_t user_bias);

/**
 * Something else changed the picture (a zoom push): restart the window, or the
 * current trial's settle, so a judgement never straddles the change.
 */
void autoexp_disturbed(AutoExp *a, uint32_t now_ms);

void autoexp_current(const AutoExp *a, AutoExpApply *out);
static inline autoexp_phase_t autoexp_phase(const AutoExp *a) { return a->phase; }

/** The effective bias of setting `index` over `user_bias`, clamped -5..0. */
int8_t autoexp_bias_for(uint8_t index, int8_t user_bias);

/**
 * The Scan screen's status line for an event, at most 21 characters:
 * "Auto exposure +2" (TRY), "Auto exposure +2 kept" (KEEP, or REVERT to a
 * setting an earlier search kept), "Auto exposure off" (KEEP or REVERT at
 * setting 0). Empty for NONE and STOP (the user can see what they just did).
 */
void autoexp_event_text(autoexp_event_t ev, uint8_t index, char *out, size_t out_size);

#endif /* LEEK_SCAN_AUTOEXP_H */
