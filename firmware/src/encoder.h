/**
 * EC11 rotary encoder: quadrature in, detent steps out.
 *
 * A state machine over the two contacts (the full-step table from Ben
 * Buxton's well-known decoder) that only reports a step after a complete,
 * valid sequence, so contact bounce on either line cannot produce a step by
 * itself, and a reversal halfway through a detent produces nothing. Pure:
 * the button task samples the pins and feeds this; the host suite feeds it
 * sequences, bounce included.
 *
 * One step per full quadrature cycle, which is one detent on the common
 * EC11 (20 detents, 20 pulses per turn). An encoder with two detents per
 * cycle (30/15) needs ENCODER_HALF_STEP.
 */
#ifndef LEEK_ENCODER_H
#define LEEK_ENCODER_H

#include <stdint.h>

typedef struct {
    uint8_t state;
} Encoder;

void encoder_init(Encoder *e);

/**
 * Feed one sample: `a` is the CLK contact, `b` the DT contact, 1 = open
 * (pulled up). Returns +1 for a clockwise detent, -1 for anticlockwise, 0
 * for nothing yet.
 */
int encoder_feed(Encoder *e, uint8_t a, uint8_t b);

#endif /* LEEK_ENCODER_H */
