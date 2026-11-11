/**
 * The air-gap fixtures shared with the host (the .txt files in vectors/ur).
 *
 * The host's UR implementation (host/leek-ur, used by the Zcash LeekWallet
 * app and zecz) wrote these: a real compact signer view as ur:leek-pczt and
 * a real .zsig as ur:leek-zsig, at the fragment sizes the companions offer.
 * This suite holds the board's side to them:
 *
 *   - ur-encoder.c, given the same message and fragment size, prints the
 *     same lines, character for character (so the board's ur:leek-zsig is
 *     what the host decoder was tested on);
 *   - ur-decoder.c reads the host's parts back to the same bytes, in
 *     order, uppercased, and joining late, inside the buffers airgap.c gives
 *     it (16 KB of fragments, 8 mixed parts of up to 512 B).
 *
 * The host's tests (host/leek-ur/tests/fixtures.rs) check the same files the
 * other way round. No board needed in either direction.
 *
 * Run from firmware/sim (make test does).
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ur-decoder.h"
#include "ur-encoder.h"

static int failures = 0;

#define CHECK(cond, ...) do {                        \
    if (!(cond)) {                                   \
        printf("  FAIL: "); printf(__VA_ARGS__);     \
        printf("\n"); failures++;                    \
    }                                                \
} while (0)

static const struct {
    const char *file;     /* vectors/ur/<file>.txt */
    const char *source;   /* vectors/<source> */
    const char *type;
    size_t max_fragment;
} FIXTURES[] = {
    { "iw_send_nu7_compact.50.leek-pczt", "pczt/iw_send_nu7_compact.pczt", "leek-pczt", 50 },
    { "main_iw_send_nu7_compact.120.leek-pczt", "pczt/main_iw_send_nu7_compact.pczt", "leek-pczt", 120 },
    { "iw_send_shielded.512.leek-zsig", "zsig/iw_send_shielded.zsig", "leek-zsig", 512 },
    { "iw_send_shielded.30.leek-zsig", "zsig/iw_send_shielded.zsig", "leek-zsig", 30 },
};

#define MAX_LINES 256
#define MAX_LINE  1200

static char lines[MAX_LINES][MAX_LINE];
static size_t n_lines;

/* The same buffers as airgap.c. */
static uint8_t fragments[16384];
static uint8_t mixed[UR_DECODER_MIXED * 512];
static UrDecoder dec;
static UrEncoder enc;

static size_t read_file(const char *path, uint8_t *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(out, 1, cap, f);
    fclose(f);
    return n;
}

static bool read_parts(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char buf[MAX_LINE];
    n_lines = 0;
    while (fgets(buf, sizeof buf, f) && n_lines < MAX_LINES) {
        buf[strcspn(buf, "\r\n")] = '\0';
        if (buf[0] == '\0' || buf[0] == '#') continue;
        memcpy(lines[n_lines++], buf, strlen(buf) + 1);
    }
    fclose(f);
    return n_lines > 0;
}

/* CBOR byte string around the payload, as airgap.c expects. */
static size_t wrap(const uint8_t *p, size_t n, uint8_t *out)
{
    size_t h;
    if (n < 24) { out[0] = (uint8_t)(0x40 | n); h = 1; }
    else if (n < 256) { out[0] = 0x58; out[1] = (uint8_t)n; h = 2; }
    else { out[0] = 0x59; out[1] = (uint8_t)(n >> 8); out[2] = (uint8_t)n; h = 3; }
    memcpy(out + h, p, n);
    return h + n;
}

/* Feed lines[from..] (`passes` times round), uppercased if asked. */
static bool decode(size_t from, int passes, bool upper, const uint8_t *want, size_t want_len)
{
    ur_decoder_init(&dec, fragments, sizeof fragments, mixed, sizeof mixed);
    for (int pass = 0; pass < passes; pass++) {
        for (size_t i = from; i < n_lines; i++) {
            char part[MAX_LINE];
            memcpy(part, lines[i], strlen(lines[i]) + 1);
            if (upper) ur_to_upper(part);
            UrPartResult r = ur_decoder_receive(&dec, part, strlen(part));
            if (r == UR_PART_REJECTED) {
                printf("  part %zu rejected\n", i + 1);
                return false;
            }
            if (r == UR_PART_COMPLETE) {
                size_t n = 0;
                const uint8_t *m = ur_decoder_message(&dec, &n);
                return m && n == want_len && memcmp(m, want, n) == 0;
            }
        }
    }
    return false;
}

static void test_fixture(size_t k)
{
    printf("== %s\n", FIXTURES[k].file);
    char path[256];
    snprintf(path, sizeof path, "../../vectors/ur/%s.txt", FIXTURES[k].file);
    CHECK(read_parts(path), "cannot read %s", path);
    if (n_lines == 0) return;

    static uint8_t payload[8192], message[8192 + 8];
    snprintf(path, sizeof path, "../../vectors/%s", FIXTURES[k].source);
    const size_t plen = read_file(path, payload, sizeof payload);
    CHECK(plen > 0, "cannot read %s", path);
    const size_t mlen = wrap(payload, plen, message);

    /* Our encoder prints the host's lines. */
    CHECK(ur_encoder_init(&enc, FIXTURES[k].type, message, mlen, FIXTURES[k].max_fragment), "init refused");
    for (size_t i = 0; i < n_lines; i++) {
        char part[MAX_LINE];
        const size_t n = ur_encoder_part(&enc, (uint32_t)(i + 1), part, sizeof part);
        CHECK(n > 0 && strcmp(part, lines[i]) == 0, "part %zu differs:\n    C    %s\n    host %s", i + 1, part, lines[i]);
    }

    /* Our decoder reads the host's parts. */
    CHECK(decode(0, 1, false, message, mlen), "in order: not decoded");
    CHECK(decode(0, 1, true, message, mlen), "uppercase: not decoded");
    if (n_lines > 1) {
        /* Joining after the first three plain parts, looping. */
        CHECK(decode(3, 3, true, message, mlen), "joining late: not decoded");
    }
}

int main(void)
{
    for (size_t k = 0; k < sizeof FIXTURES / sizeof FIXTURES[0]; k++) {
        test_fixture(k);
    }
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
