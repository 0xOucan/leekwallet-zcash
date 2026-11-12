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
 * list of settings (autoexp_setting()). Which list depends on how the code
 * looks: camera.c measures the located code's box (autoexp_measure(): mean
 * luma and the share of clipped pixels) and feeds it here (autoexp_light()).
 * A bright or clipped code - a phone at full brightness, washed out - gets
 * the darker list first: the bias 1, 2 and 3 steps under the user's, then a
 * halved shutter cap. Anything else gets the brighter list, as before: a
 * longer shutter cap, a brighter bias, the other banding frequency. (The
 * first phone test, 2026-11-11, looked overexposed at 100 % brightness, where
 * the brighter list could only make it worse.) Each is given AUTOEXP_SETTLE_MS for the
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

/* Brightness of the located code, per frame that located one ... */
/* A pixel at or above this is clipped. */
#define AUTOEXP_SAT_LEVEL            250
/* ... and the code counts as overexposed when its box has this many clipped
   pixels in 1000 (15 %: a code exposed about right clips a few whites, a
   washed-out one most of them) ... */
#define AUTOEXP_BRIGHT_SAT_PERMILLE  150
/* ... or this mean luma (half the box is dark modules; 180 means they are
   lifted well off black). */
#define AUTOEXP_BRIGHT_MEAN          180

/* The effective bias floor. The knob stops at -5; with centre-weighted
   metering camera.c's sensor level is the bias + 3, which the OV5640 takes
   down to -5, so the darker settings may go to -8 (camera_tune_ae_level()
   clamps it anyway). */
#define AUTOEXP_BIAS_MIN  (-8)

/* One setting, relative to the user's. Index 0 is the user's own. */
typedef struct {
    int8_t  bias_add;      /* added to the user's bias, then clamped AUTOEXP_BIAS_MIN..0 */
    uint8_t shutter_mul;   /* x the build's shutter cap (CAMERA_SHUTTER_CAP_LINES) ... */
    uint8_t shutter_div;   /* ... / this (1, or 2 for half the cap) */
    bool    mains_swap;    /* banding at the other mains frequency (60 <-> 50) */
} AutoExpSetting;

/* Settings 1..AUTOEXP_BRIGHTER are the brighter list ("+1".."+5" on the
   screen); AUTOEXP_BRIGHTER+1.. the darker one ("-1".."-4"). */
#define AUTOEXP_BRIGHTER  5
#define AUTOEXP_SETTINGS  10
/** Setting 0..AUTOEXP_SETTINGS-1; out of range is setting 0. */
const AutoExpSetting *autoexp_setting(uint8_t index);

/** The step a setting is called on screen and in the log: 0 for the user's,
 *  +1..+5 for the brighter list, -1..-4 for the darker. */
int autoexp_step(uint8_t index);

/** How a located code looks: mean luma 0-255 and clipped pixels in 1000. */
typedef struct {
    uint8_t  mean;
    uint16_t sat_permille;
    bool     valid;          /* false: nothing measured */
} AutoExpLight;

/**
 * Measure the box (x0,y0)-(x1,y1) of a w x h grayscale image (corners in
 * any order, clamped to the frame, x1/y1 exclusive), every other pixel of
 * every other row. Not valid for an empty box or no image.
 */
void autoexp_measure(const uint8_t *gray, uint16_t w, uint16_t h, int x0, int y0, int x1,
                     int y1, AutoExpLight *out);

/** Overexposed by the two thresholds above. */
bool autoexp_light_bright(const AutoExpLight *l);

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
    uint16_t tried_mask;       /* settings tried this search (bit per index) */
    bool     darker;           /* this search runs the darker list */
    /* The code's light at the current setting: a running average over the
       located frames since the setting last changed (light_n of them). */
    uint16_t light_mean_x16, light_sat_x16;
    uint16_t light_n;
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
    int8_t  bias;           /* effective, AUTOEXP_BIAS_MIN..0 */
    uint8_t shutter_mul;
    uint8_t shutter_div;
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
 * The light of a located code (camera.c, once a frame that located one).
 * Read when a search starts: bright or clipped picks the darker list.
 */
void autoexp_light(AutoExp *a, const AutoExpLight *l);

/** The code's light as averaged so far (not valid before a located frame). */
void autoexp_light_now(const AutoExp *a, AutoExpLight *out);

/** True while (or after) a search that chose the darker list. */
static inline bool autoexp_search_darker(const AutoExp *a) { return a->darker; }

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

/** The effective bias of setting `index` over `user_bias`, clamped
 *  AUTOEXP_BIAS_MIN..0. */
int8_t autoexp_bias_for(uint8_t index, int8_t user_bias);

/**
 * The Scan screen's status line for an event, at most 21 characters:
 * "Auto exposure +2" or "-1" (TRY, autoexp_step()), "Auto exposure +2 kept"
 * (KEEP, or REVERT to a setting an earlier search kept), "Auto exposure off"
 * (KEEP or REVERT at setting 0). Empty for NONE and STOP (the user can see what they just did).
 */
void autoexp_event_text(autoexp_event_t ev, uint8_t index, char *out, size_t out_size);

#endif /* LEEK_SCAN_AUTOEXP_H */
