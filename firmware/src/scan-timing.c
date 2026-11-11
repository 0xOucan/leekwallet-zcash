/* See scan-timing.h. */

#include "scan-timing.h"

#include <stdio.h>
#include <string.h>

static const char *const STAGE_NAME[SCAN_STAGE_COUNT] = {
    "wait", "copy", "ident", "dec", "prev", "draw",
};

void scan_timing_reset(ScanTiming *t)
{
    if (t != NULL) {
        memset(t, 0, sizeof *t);
    }
}

void scan_timing_add(ScanTiming *t, scan_stage_t stage, uint32_t us)
{
    if (t == NULL || (unsigned)stage >= SCAN_STAGE_COUNT) {
        return;
    }
    t->n[stage]++;
    t->sum_us[stage] += us;
    if (us > t->max_us[stage]) {
        t->max_us[stage] = us;
    }
}

uint32_t scan_timing_mean_us(const ScanTiming *t, scan_stage_t stage)
{
    if (t == NULL || (unsigned)stage >= SCAN_STAGE_COUNT || t->n[stage] == 0) {
        return 0;
    }
    return (uint32_t)(t->sum_us[stage] / t->n[stage]);
}

size_t scan_timing_format(const ScanTiming *t, char *out, size_t size)
{
    if (out == NULL || size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (t == NULL) {
        return 0;
    }
    size_t o = 0;
    for (int s = 0; s < SCAN_STAGE_COUNT && o + 1 < size; s++) {
        int w;
        if (t->n[s] == 0) {
            w = snprintf(out + o, size - o, "%s%s -", s ? " " : "", STAGE_NAME[s]);
        } else {
            /* Tenths of a millisecond, in integers: no float printf on the
               board's hot path. */
            const uint32_t mean = scan_timing_mean_us(t, (scan_stage_t)s);
            w = snprintf(out + o, size - o, "%s%s %u.%u/%u", s ? " " : "",
                         STAGE_NAME[s], (unsigned)(mean / 1000),
                         (unsigned)((mean % 1000) / 100),
                         (unsigned)((t->max_us[s] + 500) / 1000));
        }
        if (w < 0) {
            break;
        }
        o += (size_t)w;
        if (o >= size) {
            o = size - 1;
        }
    }
    return o;
}

void scan_ur_reset(ScanUrClock *c)
{
    if (c != NULL) {
        memset(c, 0, sizeof *c);
    }
}

void scan_ur_part(ScanUrClock *c, int64_t now_us, uint32_t frames, bool redundant)
{
    if (c == NULL) {
        return;
    }
    if (!c->running) {
        c->running = true;
        c->start_us = now_us;
        c->start_frames = frames;
        c->parts = 0;
        c->redundant = 0;
    }
    c->parts++;
    if (redundant) {
        c->redundant++;
    }
}

bool scan_ur_done(ScanUrClock *c, int64_t now_us, uint32_t frames, uint32_t seq_len,
                  ScanUrResult *out)
{
    if (c == NULL || !c->running) {
        return false;
    }
    if (out != NULL) {
        const int64_t dt = now_us - c->start_us;
        out->ms = dt > 0 ? (uint32_t)(dt / 1000) : 0;
        out->frames = frames - c->start_frames;
        out->parts = c->parts;
        out->redundant = c->redundant;
        out->seq_len = seq_len;
    }
    c->running = false;
    return true;
}

size_t scan_ur_format(const ScanUrResult *r, char *out, size_t size)
{
    if (out == NULL || size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (r == NULL) {
        return 0;
    }
    /* Parts as a percentage of seqLen: 100% is a receiver that missed
       nothing; the fountain overhead is everything above it. */
    const unsigned pct = r->seq_len ? (unsigned)((uint64_t)r->parts * 100u / r->seq_len) : 0;
    const int w = snprintf(out, size,
                           "UR complete: %u ms, %u frames, %u parts for seqLen %u "
                           "(%u redundant, %u%%)",
                           (unsigned)r->ms, (unsigned)r->frames, (unsigned)r->parts,
                           (unsigned)r->seq_len, (unsigned)r->redundant, pct);
    if (w < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)w < size ? (size_t)w : size - 1;
}
