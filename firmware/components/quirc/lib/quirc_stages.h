/* LeekWallet addition, not upstream quirc.
 *
 * Where quirc_end() spends its time, per stage, for the last frame. Compiled
 * in only with QUIRC_STAGE_TIMING (components/quirc/CMakeLists.txt turns the
 * CMake variable of the same name into a PUBLIC definition, so the caller
 * sees it too). Without it this header declares nothing and identify.c is the
 * upstream code.
 *
 * The caller supplies the clock, so the library stays free of ESP-IDF: the
 * firmware passes a microsecond timer, the host bench a nanosecond one.
 */
#ifndef QUIRC_STAGES_H_
#define QUIRC_STAGES_H_

#include <stdint.h>

#ifdef QUIRC_STAGE_TIMING

enum {
	QUIRC_STAGE_THRESHOLD = 0,	/* otsu() + pixels_setup() */
	QUIRC_STAGE_FINDER,		/* finder_scan(): runs, region fills, capstones */
	QUIRC_STAGE_GRID,		/* test_grouping() less the jiggle */
	QUIRC_STAGE_JIGGLE,		/* jiggle_perspective(), all grids */
	QUIRC_STAGE_COUNT
};

/* Set by the caller before quirc_end(); NULL disables the timing. */
extern uint32_t (*quirc_stage_clock)(void);

/* Durations of the last quirc_end(), in units of quirc_stage_clock. */
extern uint32_t quirc_stage_time[QUIRC_STAGE_COUNT];

/* perspective_map() calls in the last quirc_end(), the grid stages' unit of
   work. Counted only with QUIRC_COUNT_MAPS as well (the host bench): on the
   board the count would cost a load and a store in the hottest loop. */
extern uint32_t quirc_stage_maps;

#endif

#ifdef QUIRC_FAST_PERSPECTIVE
/* Checks the inline divide and round against __divsf3 and rintf on 2n inputs
   each; returns how many differ (identify.c). */
int quirc_fast_selftest(uint32_t n);
#endif

#endif
