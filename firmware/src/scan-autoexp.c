/* The Scan screen's automatic exposure search (scan-autoexp.h). Pure. */

#include "scan-autoexp.h"

#include <stdio.h>
#include <string.h>

/*
 * The ordered list. Longer shutter first: a PWM-dimmed OLED's bands average
 * out over more of its periods, and at half brightness the sensor already sat
 * at the 96-line cap with 4x gain, so the bias alone would only have raised
 * the gain. Then the bias up a step, which makes the AEC actually use the
 * longer cap on a brighter screen; then four times the cap (384 lines, still
 * well under the 984-line frame, so no frame stretches); last the other
 * banding step, which changes how a long exposure lines up with the PWM.
 */
/*
 * The darker list, for a code that is bright or clipped: the bias down one,
 * two and three steps (the AEC then aims lower and the white modules come
 * off the clip, the black ones back down), then two steps down with half the
 * shutter cap, for the case where the AEC sat at the cap in a dark room and
 * the bias alone would only have taken gain off.
 */
static const AutoExpSetting SETTINGS[AUTOEXP_SETTINGS] = {
    {  0, 1, 1, false },   /* 0: the user's own */
    {  0, 2, 1, false },   /* 1 (+1): shutter cap x2 */
    {  1, 2, 1, false },   /* 2 (+2): x2, bias +1 */
    {  1, 4, 1, false },   /* 3 (+3): x4, bias +1 */
    {  2, 4, 1, false },   /* 4 (+4): x4, bias +2 */
    {  1, 4, 1, true  },   /* 5 (+5): x4, bias +1, the other banding frequency */
    { -1, 1, 1, false },   /* 6 (-1): bias -1 */
    { -2, 1, 1, false },   /* 7 (-2): bias -2 */
    { -3, 1, 1, false },   /* 8 (-3): bias -3 */
    { -2, 1, 2, false },   /* 9 (-4): bias -2, half the shutter cap */
};

/* The search orders: every trial setting of the list, then the user's own
   (which only a second search, started from a kept setting, can reach). */
static const uint8_t ORDER_BRIGHTER[] = { 1, 2, 3, 4, 5, 0 };
static const uint8_t ORDER_DARKER[]   = { 6, 7, 8, 9, 0 };

const AutoExpSetting *autoexp_setting(uint8_t index)
{
    return &SETTINGS[index < AUTOEXP_SETTINGS ? index : 0];
}

int autoexp_step(uint8_t index)
{
    if (index == 0 || index >= AUTOEXP_SETTINGS) {
        return 0;
    }
    return index <= AUTOEXP_BRIGHTER ? (int)index : -(int)(index - AUTOEXP_BRIGHTER);
}

int8_t autoexp_bias_for(uint8_t index, int8_t user_bias)
{
    int b = (int)user_bias + autoexp_setting(index)->bias_add;
    return (int8_t)(b < AUTOEXP_BIAS_MIN ? AUTOEXP_BIAS_MIN : (b > 0 ? 0 : b));
}

static bool same_effect(uint8_t i, uint8_t j, int8_t user_bias)
{
    const AutoExpSetting *a = autoexp_setting(i), *b = autoexp_setting(j);
    return autoexp_bias_for(i, user_bias) == autoexp_bias_for(j, user_bias) &&
           a->shutter_mul == b->shutter_mul && a->shutter_div == b->shutter_div &&
           a->mains_swap == b->mains_swap;
}

void autoexp_measure(const uint8_t *gray, uint16_t w, uint16_t h, int x0, int y0, int x1,
                     int y1, AutoExpLight *out)
{
    memset(out, 0, sizeof *out);
    if (gray == NULL || w == 0 || h == 0) {
        return;
    }
    if (x1 < x0) { const int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { const int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)w) x1 = (int)w;
    if (y1 > (int)h) y1 = (int)h;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    uint32_t sum = 0, n = 0, sat = 0;
    for (int y = y0; y < y1; y += 2) {
        const uint8_t *row = gray + (size_t)y * w;
        for (int x = x0; x < x1; x += 2) {
            const uint8_t v = row[x];
            sum += v;
            sat += v >= AUTOEXP_SAT_LEVEL;
            n++;
        }
    }
    out->mean = (uint8_t)(sum / n);
    out->sat_permille = (uint16_t)(sat * 1000u / n);
    out->valid = true;
}

bool autoexp_light_bright(const AutoExpLight *l)
{
    return l != NULL && l->valid &&
           (l->sat_permille >= AUTOEXP_BRIGHT_SAT_PERMILLE || l->mean >= AUTOEXP_BRIGHT_MEAN);
}

static void light_clear(AutoExp *a)
{
    a->light_mean_x16 = 0;
    a->light_sat_x16 = 0;
    a->light_n = 0;
}

void autoexp_light(AutoExp *a, const AutoExpLight *l)
{
    if (l == NULL || !l->valid) {
        return;
    }
    const uint16_t m = (uint16_t)(l->mean * 16u);
    const uint16_t sp = (uint16_t)((l->sat_permille > 1000 ? 1000 : l->sat_permille) * 16u / 4u);
    if (a->light_n == 0) {
        a->light_mean_x16 = m;
        a->light_sat_x16 = sp;
    } else {
        /* An eighth of each new frame: a second or two of codes. */
        a->light_mean_x16 = (uint16_t)(((uint32_t)a->light_mean_x16 * 7u + m) / 8u);
        a->light_sat_x16 = (uint16_t)(((uint32_t)a->light_sat_x16 * 7u + sp) / 8u);
    }
    if (a->light_n < 0xFFFF) {
        a->light_n++;
    }
}

void autoexp_light_now(const AutoExp *a, AutoExpLight *out)
{
    out->valid = a->light_n > 0;
    out->mean = (uint8_t)(a->light_mean_x16 / 16u);
    out->sat_permille = (uint16_t)(a->light_sat_x16 * 4u / 16u);
}

/* Called wherever the setting (or the picture) changes, so the light
   average restarts with it. */
static void light_clear(AutoExp *a);

static void ring_clear(AutoExp *a)
{
    a->ring_n = 0;
    a->ring_head = 0;
    light_clear(a);
}

static void ring_push(AutoExp *a, uint32_t now, uint32_t located, uint32_t decoded)
{
    if (a->ring_n > 0) {
        const AutoExpSample *last =
            &a->ring[(a->ring_head + AUTOEXP_RING - 1) % AUTOEXP_RING];
        if (now - last->t_ms < AUTOEXP_SAMPLE_MS) {
            return;
        }
    }
    a->ring[a->ring_head] = (AutoExpSample){ now, located, decoded };
    a->ring_head = (uint8_t)((a->ring_head + 1) % AUTOEXP_RING);
    if (a->ring_n < AUTOEXP_RING) {
        a->ring_n++;
    }
}

/* The newest sample at least a window old: false until there is one. */
static bool ring_window(const AutoExp *a, uint32_t now, AutoExpSample *out)
{
    for (uint8_t k = 1; k <= a->ring_n; k++) {
        const AutoExpSample *s = &a->ring[(a->ring_head + AUTOEXP_RING - k) % AUTOEXP_RING];
        if (now - s->t_ms >= AUTOEXP_WINDOW_MS) {
            *out = *s;
            return true;
        }
    }
    return false;
}

static uint16_t score_of(uint32_t located, uint32_t decoded)
{
    if (located == 0) {
        return 0;
    }
    const uint64_t p = (uint64_t)decoded * 1000u / located;
    return (uint16_t)(p > 1000 ? 1000 : p);
}

void autoexp_reset(AutoExp *a, uint32_t now_ms, int8_t user_bias)
{
    memset(a, 0, sizeof *a);
    a->phase = AUTOEXP_IDLE;
    a->user_bias = user_bias;
    a->phase_ms = now_ms;
    a->best = 0xFF;
}

static void enter(AutoExp *a, autoexp_phase_t phase, uint32_t now)
{
    a->phase = phase;
    a->phase_ms = now;
    a->extended = 0;
}

static autoexp_event_t finish_search(AutoExp *a, uint32_t now)
{
    ring_clear(a);
    if (a->best != 0xFF && a->best_decoded >= AUTOEXP_KEEP_MIN_DECODED &&
        a->best_score >= (uint32_t)a->origin_score + AUTOEXP_MARGIN_PERMILLE) {
        a->current = a->best;
        enter(a, AUTOEXP_IDLE, now);
        return AUTOEXP_EV_KEEP;
    }
    a->current = a->origin;
    enter(a, AUTOEXP_COOLDOWN, now);
    return AUTOEXP_EV_REVERT;
}

static autoexp_event_t next_trial(AutoExp *a, uint32_t now)
{
    const uint8_t *order = a->darker ? ORDER_DARKER : ORDER_BRIGHTER;
    const uint8_t len = a->darker ? (uint8_t)sizeof ORDER_DARKER : (uint8_t)sizeof ORDER_BRIGHTER;
    while (a->next_pos < len) {
        const uint8_t c = order[a->next_pos++];
        bool dup = false;
        for (uint8_t t = 0; t < AUTOEXP_SETTINGS && !dup; t++) {
            dup = (a->tried_mask & (1u << t)) && same_effect(c, t, a->user_bias);
        }
        if (dup) {
            continue;
        }
        a->tried_mask |= (uint16_t)(1u << c);
        a->current = c;
        ring_clear(a);
        enter(a, AUTOEXP_SETTLING, now);
        return AUTOEXP_EV_TRY;
    }
    return finish_search(a, now);
}

autoexp_event_t autoexp_tick(AutoExp *a, uint32_t now, uint32_t located, uint32_t decoded)
{
    switch (a->phase) {
    case AUTOEXP_IDLE: {
        ring_push(a, now, located, decoded);
        AutoExpSample old;
        if (!ring_window(a, now, &old)) {
            return AUTOEXP_EV_NONE;
        }
        const uint32_t loc = located - old.located;
        const uint32_t dec = decoded - old.decoded;
        if (loc < AUTOEXP_TRIGGER_MIN ||
            (uint64_t)dec * 1000u >= (uint64_t)loc * AUTOEXP_TRIGGER_PERMILLE) {
            return AUTOEXP_EV_NONE;
        }
        a->last_located = loc;
        a->last_decoded = dec;
        a->last_score = score_of(loc, dec);
        if (a->searches >= AUTOEXP_MAX_SEARCHES) {
            enter(a, AUTOEXP_DONE, now);
            return AUTOEXP_EV_NONE;
        }
        a->searches++;
        a->origin = a->current;
        a->origin_score = a->last_score;
        a->best = 0xFF;
        a->best_score = 0;
        a->best_decoded = 0;
        a->tried_mask = (uint16_t)(1u << a->origin);
        a->next_pos = 0;
        /* Which way: the code's light at the setting that triggered. */
        AutoExpLight l;
        autoexp_light_now(a, &l);
        a->darker = autoexp_light_bright(&l);
        return next_trial(a, now);
    }
    case AUTOEXP_SETTLING:
        if (now - a->phase_ms >= AUTOEXP_SETTLE_MS) {
            enter(a, AUTOEXP_JUDGING, now);
            a->mark_located = located;
            a->mark_decoded = decoded;
        }
        return AUTOEXP_EV_NONE;
    case AUTOEXP_JUDGING: {
        if (now - a->phase_ms < (uint32_t)AUTOEXP_HOLD_MS * (1u + a->extended)) {
            return AUTOEXP_EV_NONE;
        }
        const uint32_t loc = located - a->mark_located;
        const uint32_t dec = decoded - a->mark_decoded;
        if (loc < AUTOEXP_JUDGE_MIN && !a->extended) {
            a->extended = 1;
            return AUTOEXP_EV_NONE;
        }
        const uint16_t score = loc >= AUTOEXP_JUDGE_MIN ? score_of(loc, dec) : 0;
        a->last_located = loc;
        a->last_decoded = dec;
        a->last_score = score;
        if (a->best == 0xFF || score > a->best_score) {
            a->best = a->current;
            a->best_score = score;
            a->best_decoded = dec;
        }
        if (score >= AUTOEXP_GOOD_PERMILLE && dec >= AUTOEXP_KEEP_MIN_DECODED &&
            score >= (uint32_t)a->origin_score + AUTOEXP_MARGIN_PERMILLE) {
            ring_clear(a);
            enter(a, AUTOEXP_IDLE, now);
            return AUTOEXP_EV_KEEP;
        }
        return next_trial(a, now);
    }
    case AUTOEXP_COOLDOWN:
        if (now - a->phase_ms >= AUTOEXP_COOLDOWN_MS) {
            ring_clear(a);
            enter(a, AUTOEXP_IDLE, now);
        }
        return AUTOEXP_EV_NONE;
    case AUTOEXP_DONE:
    case AUTOEXP_STOPPED:
    default:
        return AUTOEXP_EV_NONE;
    }
}

autoexp_event_t autoexp_user_override(AutoExp *a, int8_t user_bias)
{
    a->user_bias = user_bias;
    if (a->phase == AUTOEXP_STOPPED) {
        return AUTOEXP_EV_NONE;
    }
    a->phase = AUTOEXP_STOPPED;
    a->current = 0;
    ring_clear(a);
    return AUTOEXP_EV_STOP;
}

void autoexp_disturbed(AutoExp *a, uint32_t now_ms)
{
    switch (a->phase) {
    case AUTOEXP_IDLE:
        ring_clear(a);
        break;
    case AUTOEXP_SETTLING:
    case AUTOEXP_JUDGING:
        enter(a, AUTOEXP_SETTLING, now_ms);
        break;
    default:
        break;
    }
}

void autoexp_current(const AutoExp *a, AutoExpApply *out)
{
    const AutoExpSetting *s = autoexp_setting(a->current);
    out->index = a->current;
    out->bias = autoexp_bias_for(a->current, a->user_bias);
    out->shutter_mul = s->shutter_mul;
    out->shutter_div = s->shutter_div;
    out->mains_swap = s->mains_swap;
}

void autoexp_event_text(autoexp_event_t ev, uint8_t index, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    switch (ev) {
    case AUTOEXP_EV_TRY:
        snprintf(out, out_size, "Auto exposure %+d", autoexp_step(index));
        break;
    case AUTOEXP_EV_KEEP:
    case AUTOEXP_EV_REVERT:
        if (index == 0) {
            snprintf(out, out_size, "Auto exposure off");
        } else {
            snprintf(out, out_size, "Auto exposure %+d kept", autoexp_step(index));
        }
        break;
    default:
        out[0] = '\0';
        break;
    }
}
