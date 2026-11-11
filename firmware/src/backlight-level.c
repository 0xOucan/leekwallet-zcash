/* The colour panel's light levels. See backlight-level.h. */

#include "backlight-level.h"

#include <stdio.h>

/* Ticks of 16383, rounded from the percent on the label; the bottom one is
   the single tick, 0.0061 %. */
static const struct {
    uint16_t    duty;
    const char *label;
} LEVELS[BL_LEVEL_COUNT] = {
    { 1, "0.006%" }, { 2, "0.01%" }, { 3, "0.02%" }, { 5, "0.03%" }, { 8, "0.05%" },
    { 11, "0.07%" }, { 16, "0.1%" }, { 33, "0.2%" }, { 49, "0.3%" }, { 82, "0.5%" },
    { 115, "0.7%" },
    { 164, "1%" }, { 328, "2%" }, { 491, "3%" }, { 819, "5%" }, { 1147, "7%" },
    { 1638, "10%" }, { 2457, "15%" }, { 3277, "20%" }, { 4915, "30%" }, { 6553, "40%" },
    { 8192, "50%" }, { 9830, "60%" }, { 11468, "70%" }, { 13106, "80%" }, { 14745, "90%" },
    { BL_DUTY_MAX, "100%" },
};

uint16_t bl_level_duty(uint8_t level)
{
    return LEVELS[level < BL_LEVEL_COUNT ? level : BL_LEVEL_FULL].duty;
}

uint8_t bl_level_step(uint8_t level, int dir)
{
    if (level > BL_LEVEL_FULL) {
        level = BL_LEVEL_FULL;
    }
    if (dir > 0 && level < BL_LEVEL_FULL) {
        return (uint8_t)(level + 1);
    }
    if (dir < 0 && level > 0) {
        return (uint8_t)(level - 1);
    }
    return level;
}

uint8_t bl_level_from_pct(unsigned pct)
{
    if (pct == 0 || pct > 100) {
        return BL_LEVEL_DEFAULT;
    }
    /* The smallest ratio between the percent and a level's, in hundredths
       of a percent so the levels below 1 % compare too. */
    const uint32_t want = (uint32_t)pct * BL_DUTY_MAX;   /* duty x 100 */
    uint8_t best = BL_LEVEL_1PCT;
    uint32_t best_ratio = UINT32_MAX;
    for (uint8_t i = 0; i < BL_LEVEL_COUNT; i++) {
        const uint32_t d = (uint32_t)LEVELS[i].duty * 100;
        const uint32_t ratio = d > want ? (uint32_t)((uint64_t)d * 1024 / want)
                                        : (uint32_t)((uint64_t)want * 1024 / d);
        if (ratio < best_ratio) {
            best_ratio = ratio;
            best = i;
        }
    }
    return best;
}

void bl_level_label(uint8_t level, char *out, size_t n)
{
    snprintf(out, n, "%s", LEVELS[level < BL_LEVEL_COUNT ? level : BL_LEVEL_FULL].label);
}
