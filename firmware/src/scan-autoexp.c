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
static const AutoExpSetting SETTINGS[AUTOEXP_SETTINGS] = {
    { 0, 1, false },   /* 0: the user's own */
    { 0, 2, false },   /* 1: shutter cap x2 */
    { 1, 2, false },   /* 2: x2, bias +1 */
    { 1, 4, false },   /* 3: x4, bias +1 */
    { 2, 4, false },   /* 4: x4, bias +2 */
    { 1, 4, true  },   /* 5: x4, bias +1, the other banding frequency */
};

/* The search order: every trial setting, then the user's own (which only a
   second search, started from a kept setting, can reach). */
static const uint8_t ORDER[AUTOEXP_SETTINGS] = { 1, 2, 3, 4, 5, 0 };

const AutoExpSetting *autoexp_setting(uint8_t index)
{
    return &SETTINGS[index < AUTOEXP_SETTINGS ? index : 0];
}

int8_t autoexp_bias_for(uint8_t index, int8_t user_bias)
{
    int b = (int)user_bias + autoexp_setting(index)->bias_add;
    return (int8_t)(b < -5 ? -5 : (b > 0 ? 0 : b));
}

static bool same_effect(uint8_t i, uint8_t j, int8_t user_bias)
{
    const AutoExpSetting *a = autoexp_setting(i), *b = autoexp_setting(j);
    return autoexp_bias_for(i, user_bias) == autoexp_bias_for(j, user_bias) &&
           a->shutter_mul == b->shutter_mul && a->mains_swap == b->mains_swap;
}

static void ring_clear(AutoExp *a)
{
    a->ring_n = 0;
    a->ring_head = 0;
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
    while (a->next_pos < AUTOEXP_SETTINGS) {
        const uint8_t c = ORDER[a->next_pos++];
        bool dup = false;
        for (uint8_t t = 0; t < AUTOEXP_SETTINGS && !dup; t++) {
            dup = (a->tried_mask & (1u << t)) && same_effect(c, t, a->user_bias);
        }
        if (dup) {
            continue;
        }
        a->tried_mask |= (uint8_t)(1u << c);
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
        a->tried_mask = (uint8_t)(1u << a->origin);
        a->next_pos = 0;
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
    out->mains_swap = s->mains_swap;
}

void autoexp_event_text(autoexp_event_t ev, uint8_t index, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    switch (ev) {
    case AUTOEXP_EV_TRY:
        snprintf(out, out_size, "Auto exposure +%u", (unsigned)index);
        break;
    case AUTOEXP_EV_KEEP:
    case AUTOEXP_EV_REVERT:
        if (index == 0) {
            snprintf(out, out_size, "Auto exposure off");
        } else {
            snprintf(out, out_size, "Auto exposure +%u kept", (unsigned)index);
        }
        break;
    default:
        out[0] = '\0';
        break;
    }
}
