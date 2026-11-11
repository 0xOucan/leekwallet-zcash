/* Where quirc_end() spends its time, on the host, by stage.
 *
 * Built against the firmware's vendored quirc (components/quirc/lib) with the
 * firmware's own flags (single-precision float, tgmath) plus
 * QUIRC_STAGE_TIMING, and optionally QUIRC_FAST_PERSPECTIVE, so the two
 * builds can be compared frame by frame:
 *
 *   make            # bench (upstream maths) and bench-fast
 *   make run        # both over v5/v6/v8/none frames, then a result diff
 *
 * Host milliseconds are not board milliseconds (an x86 core is tens of times
 * faster and has no PSRAM); what carries over is the SPLIT between stages and
 * the operation counts, which are the same code on the same pixels.
 *
 * With -v it prints one line per frame: located, decoded, the grid corners
 * and an FNV hash of the payload, for diffing the two builds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "quirc.h"
#include "quirc_stages.h"

#define W 320
#define H 240

static uint32_t clock_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec);
}

static uint32_t fnv(const uint8_t *p, int n)
{
	uint32_t h = 2166136261u;
	while (n-- > 0) {
		h ^= *p++;
		h *= 16777619u;
	}
	return h;
}

int main(int argc, char **argv)
{
	int verbose = 0, reps = 5, argi = 1;
	if (argi < argc && strcmp(argv[argi], "-v") == 0) {
		verbose = 1;
		argi++;
	}
	if (argi >= argc) {
		fprintf(stderr, "usage: %s [-v] frames.gray [reps]\n", argv[0]);
		return 1;
	}
	const char *path = argv[argi++];
	if (argi < argc)
		reps = atoi(argv[argi]);

	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	const long total = ftell(f);
	fseek(f, 0, SEEK_SET);
	const int frames = (int)(total / (W * H));
	uint8_t *data = malloc((size_t)total);
	if (!data || fread(data, 1, (size_t)total, f) != (size_t)total)
		return 1;
	fclose(f);

	struct quirc *q = quirc_new();
	if (!q || quirc_resize(q, W, H) < 0)
		return 1;
	quirc_stage_clock = clock_ns;

	static struct quirc_code code;
	static struct quirc_data out;
	double stage_ns[QUIRC_STAGE_COUNT] = {0}, ident_ns = 0, maps = 0;
	int located = 0, decoded = 0, runs = 0;
	int errs[8] = {0};

	for (int r = 0; r < reps; r++) {
		for (int i = 0; i < frames; i++) {
			uint8_t *img = quirc_begin(q, NULL, NULL);
			memcpy(img, data + (long)i * W * H, W * H);
			const uint32_t t0 = clock_ns();
			quirc_end(q);
			ident_ns += (double)(clock_ns() - t0);
			for (int s = 0; s < QUIRC_STAGE_COUNT; s++)
				stage_ns[s] += quirc_stage_time[s];
			maps += quirc_stage_maps;
			runs++;
			const int n = quirc_count(q);
			located += n;
			for (int j = 0; j < n; j++) {
				quirc_extract(q, j, &code);
				const quirc_decode_error_t e = quirc_decode(&code, &out);
				errs[e < 8 ? e : 7]++;
				if (e == QUIRC_SUCCESS)
					decoded++;
				if (verbose && r == 0)
					printf("frame %2d grid %d size %d corners %d,%d %d,%d %d,%d %d,%d "
					       "-> %s %08x\n", i, j, code.size,
					       code.corners[0].x, code.corners[0].y,
					       code.corners[1].x, code.corners[1].y,
					       code.corners[2].x, code.corners[2].y,
					       code.corners[3].x, code.corners[3].y,
					       quirc_strerror(e),
					       e == QUIRC_SUCCESS ? fnv(out.payload, out.payload_len) : 0);
			}
			if (verbose && r == 0 && n == 0)
				printf("frame %2d none\n", i);
		}
	}

	static const char *names[] = {"threshold", "finder", "grid", "jiggle"};
	if (verbose)
		goto done;	/* per-frame lines only, so two builds diff cleanly */
	printf("%-10s %d frames x %d: ident %.3f ms/frame |", path, frames, reps,
	       ident_ns / runs / 1e6);
	for (int s = 0; s < QUIRC_STAGE_COUNT; s++)
		printf(" %s %.3f (%.0f%%)", names[s], stage_ns[s] / runs / 1e6,
		       100.0 * stage_ns[s] / ident_ns);
	printf(" | maps %.0f/frame | located %d decoded %d per rep", maps / runs,
	       located / reps, decoded / reps);
	printf(" | errors:");
	for (int e = 1; e < 8; e++)
		if (errs[e])
			printf(" %s x%d", quirc_strerror((quirc_decode_error_t)e), errs[e] / reps);
	printf("\n");
done:
	quirc_destroy(q);
	free(data);
	return 0;
}
