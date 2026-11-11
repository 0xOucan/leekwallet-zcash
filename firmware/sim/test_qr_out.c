/**
 * The QR return path's schedule, against the real QR encoder.
 *
 * The claim qr-out.c makes is that every frame it will ever hand the panel
 * fits the mode's version. Here every frame of a full cycle, in every mode,
 * for the message sizes the device actually sends (a signature, an xpub) and
 * the largest it accepts, is fed to qrcode_initText at that version - the
 * exact call oled_draw_qrcode_at() makes - and then read back by ur-decoder.c.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "qr-out.h"
#include "qrcode.h"
#include "ur-decoder.h"

static int failures = 0;

#define CHECK(cond, ...) do {                        \
    if (!(cond)) {                                   \
        printf("  FAIL: "); printf(__VA_ARGS__);     \
        printf("\n"); failures++;                    \
    }                                                \
} while (0)

static uint8_t fragments[8 * 1024];
static uint8_t mixed[8 * 1024];
static UrDecoder dec;

static void test_every_frame_fits_and_decodes(void)
{
    printf("== every frame of every mode fits its version and decodes\n");

    static const size_t LENGTHS[] = { 1, 20, 87, 90, 104, 150, QR_OUT_MAX_MESSAGE };
    uint8_t msg[QR_OUT_MAX_MESSAGE];
    for (size_t i = 0; i < sizeof msg; i++) {
        msg[i] = (uint8_t)(i * 29 + 1);
    }

    for (uint8_t mode = 0; mode < QR_OUT_MODE_COUNT; mode++) {
        const QrOutMode *m = qr_out_mode_info(mode);
        for (size_t li = 0; li < sizeof LENGTHS / sizeof LENGTHS[0]; li++) {
            const size_t len = LENGTHS[li];
            int64_t now = 0;
            const bool started = qr_out_start("eth-signature", msg, len, mode, now);
            /* Version 3 at scale 2 is the mode section 32 warned about: 77
               characters, most of them envelope. It carries what the device
               actually sends - a ~90-byte signature, a ~110-byte xpub - and
               must refuse, not truncate, anything it cannot. */
            const bool must = m->version > 3 || len <= 150;
            CHECK(started || !must, "%zu bytes would not start in mode %s", len, m->name);
            if (!started) {
                CHECK(qr_out_frame() == NULL, "a refused start left a frame up");
                continue;
            }
            ur_decoder_init(&dec, fragments, sizeof fragments, mixed, sizeof mixed);
            bool complete = false;

            for (uint32_t f = 0; f < QR_OUT_SEQ_WRAP + 3; f++) {
                const char *frame = qr_out_frame();
                uint8_t grid[512];
                QRCode qr;
                const uint8_t version = qr_out_current_version();
                CHECK(m->version == 0 || version == m->version, "mode %s drew at v%u",
                      m->name, version);
                CHECK(frame != NULL && qrcode_initText(&qr, grid, version, ECC_LOW, frame) == 0,
                      "mode %s, %zu bytes, part %u (%zu chars) does not fit version %u",
                      m->name, len, qr_out_seq_num(), frame ? strlen(frame) : 0,
                      version);
                if (frame != NULL && !complete) {
                    const UrPartResult r = ur_decoder_receive(&dec, frame, strlen(frame));
                    CHECK(r != UR_PART_REJECTED, "the decoder refused part %u",
                          qr_out_seq_num());
                    complete = (r == UR_PART_COMPLETE);
                }
                if (qr_out_seq_len() == 1) {
                    CHECK(!qr_out_tick(now + 10000000), "a static code advanced");
                    break;
                }
                /* Not due yet: the frame stays. */
                CHECK(!qr_out_tick(now + 1), "advanced before its period");
                now += (int64_t)m->period_ms * 1000;
                CHECK(qr_out_tick(now), "did not advance after its period");
            }
            size_t out_len = 0;
            const uint8_t *out = ur_decoder_message(&dec, &out_len);
            CHECK(complete && out != NULL && out_len == len && memcmp(out, msg, len) == 0,
                  "mode %s, %zu bytes did not reassemble", m->name, len);
        }
    }
    qr_out_stop();
}

static void test_the_wrap(void)
{
    printf("== the sequence wraps before a part can outgrow its frame\n");

    uint8_t msg[120] = {0};
    CHECK(qr_out_start("crypto-hdkey", msg, sizeof msg, 2, 0), "did not start");
    int64_t now = 0;
    uint32_t last = 0;
    for (uint32_t i = 0; i < QR_OUT_SEQ_WRAP; i++) {
        last = qr_out_seq_num();
        now += 1000000;
        qr_out_tick(now);
    }
    CHECK(last == QR_OUT_SEQ_WRAP && qr_out_seq_num() == 1,
          "after part %u came %u, not 1", last, qr_out_seq_num());
    qr_out_stop();
}

static void test_refusals_and_modes(void)
{
    printf("== a mode switch restarts; oversized input shows nothing\n");

    uint8_t msg[QR_OUT_MAX_MESSAGE + 1] = {0};
    CHECK(!qr_out_start("eth-signature", msg, sizeof msg, 0, 0),
          "a message over QR_OUT_MAX_MESSAGE started");
    CHECK(qr_out_frame() == NULL, "a refused start left a frame up");
    CHECK(!qr_out_start("Bad Type", msg, 10, 0, 0), "an invalid type started");
    CHECK(!qr_out_start("eth-signature", msg, 10, QR_OUT_MODE_COUNT, 0),
          "a mode past the table started");

    CHECK(qr_out_start("eth-signature", msg, 90, 0, 0), "did not start");
    qr_out_tick(10000000);
    CHECK(qr_out_seq_num() == 2, "did not advance");
    CHECK(qr_out_set_mode(2, 10000000) && qr_out_seq_num() == 1 &&
          qr_out_current_mode() == 2, "a mode switch did not restart at part 1");
    CHECK(!qr_out_set_mode(QR_OUT_MODE_COUNT, 0) && qr_out_current_mode() == 2 &&
          qr_out_frame() != NULL,
          "a bad mode switch blanked the running one");

    /* Section 32's arithmetic, now measured: a 90-byte signature is one
       frame at version 10 and several at version 3 scale 2. */
    qr_out_set_mode(1, 0);
    const uint32_t dense = qr_out_seq_len();
    qr_out_set_mode(2, 0);
    const uint32_t chunky = qr_out_seq_len();
    printf("    90-byte eth-signature: v10 %u frame(s), v6 ", dense);
    qr_out_set_mode(0, 0);
    printf("%u, v3x2 %u\n", qr_out_seq_len(), chunky);
    CHECK(dense == 1, "a signature needs %u frames at version 10", dense);
    CHECK(chunky > 1, "version 3 at scale 2 held a whole signature");

    qr_out_stop();
    CHECK(qr_out_frame() == NULL, "stop left a frame up");
}

/* QR_OUT_MODE_STILL: one part at the smallest version that holds it. A
 * .zsig reply for one transparent input is 108 bytes (qr-out.h). */
static void test_still(void)
{
    printf("== still: one code, smallest version\n");
    uint8_t msg[QR_OUT_MAX_MESSAGE] = {0};
    for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(i * 13 + 7);

    CHECK(qr_out_start("leek-zsig", msg, 108, QR_OUT_MODE_STILL, 0), "108 bytes did not start");
    const uint8_t v = qr_out_current_version();
    printf("    108-byte reply: one v%u code (%zu chars)\n", v, strlen(qr_out_frame()));
    CHECK(qr_out_seq_len() == 1, "a still code has %u parts", qr_out_seq_len());
    CHECK(v >= 1 && v <= 8, "108 bytes needed v%u", v);
    CHECK(qr_out_alnum_capacity(v - 1) < strlen(qr_out_frame()), "v%u was not the smallest", v);
    CHECK(!qr_out_tick(100000000), "a still code advanced");

    CHECK(!qr_out_start("leek-zsig", msg, 400, QR_OUT_MODE_STILL, 0),
          "400 bytes claimed to fit one code");
    CHECK(qr_out_frame() == NULL, "a refused still start left a frame up");
    qr_out_stop();
}

/* The colour panel's modes (qr-out.h): never on an OLED's list, each bigger
 * version carrying a 108-byte .zsig in fewer frames. */
static void test_colour_modes(void)
{
    printf("== colour modes: a .zsig in fewer frames as the version grows\n");
    uint8_t msg[108];
    for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(i * 13 + 7);
    static const uint8_t ORDER[] = { QR_OUT_MODE_C_V3, QR_OUT_MODE_C_V4, QR_OUT_MODE_C_V6,
                                     QR_OUT_MODE_C_V9 };
    uint32_t prev = UINT32_MAX;
    printf("   ");
    for (size_t k = 0; k < sizeof ORDER; k++) {
        const QrOutMode *m = qr_out_mode_info(ORDER[k]);
        CHECK(m->color && !m->inverted, "%s is not a colour mode", m->name);
        CHECK(qr_out_start("leek-zsig", msg, sizeof msg, ORDER[k], 0), "%s did not start", m->name);
        printf(" %s %u", m->name, qr_out_seq_len());
        CHECK(qr_out_seq_len() < prev, "%s needs %u frames", m->name, qr_out_seq_len());
        prev = qr_out_seq_len();
    }
    printf("\n");
    CHECK(prev == 1, "v9 needs %u frames for a .zsig", prev);
    CHECK(QR_OUT_COLOR_MODE_DEFAULT == QR_OUT_MODE_C_V6, "the colour default moved");
    for (uint8_t mode = 0; mode < QR_OUT_MODE_C_V9; mode++) {
        CHECK(!qr_out_mode_info(mode)->color, "an OLED index is a colour mode");
    }
    qr_out_stop();
}

int main(void)
{
    test_colour_modes();
    test_every_frame_fits_and_decodes();
    test_still();
    test_the_wrap();
    test_refusals_and_modes();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
