/**
 * EC11 decoding. See encoder.h.
 */
#include "encoder.h"

#define DIR_CW  0x10
#define DIR_CCW 0x20

#ifndef ENCODER_HALF_STEP
/* Full step: rest is both contacts open (0b11). */
enum { R_START, R_CW_FINAL, R_CW_BEGIN, R_CW_NEXT, R_CCW_BEGIN, R_CCW_FINAL, R_CCW_NEXT };
static const uint8_t TABLE[7][4] = {
    /* R_START */     { R_START,    R_CW_BEGIN,  R_CCW_BEGIN, R_START },
    /* R_CW_FINAL */  { R_CW_NEXT,  R_START,     R_CW_FINAL,  R_START | DIR_CW },
    /* R_CW_BEGIN */  { R_CW_NEXT,  R_CW_BEGIN,  R_START,     R_START },
    /* R_CW_NEXT */   { R_CW_NEXT,  R_CW_BEGIN,  R_CW_FINAL,  R_START },
    /* R_CCW_BEGIN */ { R_CCW_NEXT, R_START,     R_CCW_BEGIN, R_START },
    /* R_CCW_FINAL */ { R_CCW_NEXT, R_CCW_FINAL, R_START,     R_START | DIR_CCW },
    /* R_CCW_NEXT */  { R_CCW_NEXT, R_CCW_FINAL, R_CCW_BEGIN, R_START },
};
#define STATES 7
#else
/* Half step: a detent at both 0b00 and 0b11. */
enum { R_START, R_CCW_BEGIN, R_CW_BEGIN, R_START_M, R_CW_BEGIN_M, R_CCW_BEGIN_M };
static const uint8_t TABLE[6][4] = {
    /* R_START */       { R_START_M,           R_CW_BEGIN,     R_CCW_BEGIN,     R_START },
    /* R_CCW_BEGIN */   { R_START_M | DIR_CCW, R_START,        R_CCW_BEGIN,     R_START },
    /* R_CW_BEGIN */    { R_START_M | DIR_CW,  R_CW_BEGIN,     R_START,         R_START },
    /* R_START_M */     { R_START_M,           R_CCW_BEGIN_M,  R_CW_BEGIN_M,    R_START },
    /* R_CW_BEGIN_M */  { R_START_M,           R_START_M,      R_CW_BEGIN_M,    R_START | DIR_CW },
    /* R_CCW_BEGIN_M */ { R_START_M,           R_CCW_BEGIN_M,  R_START_M,       R_START | DIR_CCW },
};
#define STATES 6
#endif

void encoder_init(Encoder *e)
{
    e->state = R_START;
}

int encoder_feed(Encoder *e, uint8_t a, uint8_t b)
{
    const uint8_t pins = (uint8_t)(((b & 1u) << 1) | (a & 1u));
    if ((e->state & 0x0F) >= STATES) e->state = R_START;
    e->state = TABLE[e->state & 0x0F][pins];
    if (e->state & DIR_CW) return 1;
    if (e->state & DIR_CCW) return -1;
    return 0;
}
