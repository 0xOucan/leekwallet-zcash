/* The 128x128 SH1107 panel image (src/panel128.c).
 *
 * The UI frame is stretched 2x vertically over all 128 rows; every QR the
 * device draws on 64 rows is redrawn across the full panel at a larger
 * scale, and still scans back to exactly what was drawn (zbarimg). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "oled.h"
#include "oled-core.h"
#include "panel128.h"
#include "../../vectors/keys_vectors.h"

void leek_log(const char *level, const char *tag, const char *fmt, ...) { }
esp_err_t oled_refresh(void) { return ESP_OK; }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static uint8_t img[PANEL128_BYTES];
static bool have_zbar;

static bool px(int x, int y) { return (img[(y / 8) * 128 + x] >> (y % 8)) & 1; }

static bool scans_to(const char *want)
{
    if (!have_zbar) return true;
    char path[] = "/tmp/leek-panel128-XXXXXX";
    int fd = mkstemp(path);
    FILE *f = fdopen(fd, "wb");
    const int S = 3, M = 24, W = 128 * S + 2 * M;
    fprintf(f, "P5\n%d %d\n255\n", W, W);
    for (int y = 0; y < W; y++) {
        for (int x = 0; x < W; x++) {
            int qx = (x - M) / S, qy = (y - M) / S;
            fputc(x >= M && y >= M && qx < 128 && qy < 128 && px(qx, qy) ? 255 : 0, f);
        }
    }
    fclose(f);
    char cmd[128], got[512] = {0};
    snprintf(cmd, sizeof cmd, "zbarimg -q --raw %s 2>/dev/null", path);
    FILE *z = popen(cmd, "r");
    size_t n = z ? fread(got, 1, sizeof got - 1, z) : 0;
    if (z) pclose(z);
    unlink(path);
    while (n && (got[n - 1] == '\n')) got[--n] = 0;
    if (strcmp(got, want)) printf("    scanned \"%s\"\n", got);
    return strcmp(got, want) == 0;
}

int main(void)
{
    have_zbar = system("command -v zbarimg >/dev/null 2>&1") == 0;
    printf("== test_panel128%s\n", have_zbar ? "" : " (zbarimg missing: scans SKIPPED)");

    /* Text: the frame, every row drawn twice, over all 16 pages. */
    oled_clear();
    oled_draw_string(0, 0, "Menu  ZFP 57F3A27A");
    oled_draw_string(7, 0, "UP DN       SEL");
    bool inv;
    CHECK(oled_core_last_qr(&inv) == NULL, "a QR remembered after a clear");
    panel128_compose(oled_core_framebuffer(), NULL, false, img);
    const uint8_t *fb = oled_core_framebuffer();
    int wrong = 0, lit = 0;
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 128; x++) {
            const bool want = (fb[(y / 2 / 8) * 128 + x] >> ((y / 2) % 8)) & 1;
            if (px(x, y) != want) wrong++;
            lit += px(x, y);
        }
    }
    CHECK(wrong == 0, "%d panel pixels differ from the doubled frame", wrong);
    CHECK(lit > 0, "nothing lit");
    /* Top text row reaches the panel's top rows, bottom row its last ones. */
    bool top = false, bottom = false;
    for (int x = 0; x < 128; x++) {
        top |= px(x, 0) || px(x, 2);
        bottom |= px(x, 113) || px(x, 115) || px(x, 120);
    }
    CHECK(top && bottom, "the frame does not span the panel");

    /* Every vector's UA and t-address, at the 64-row choice, redrawn. */
    CHECK(panel128_qr_scale(45) == 2, "a v7 UA at 2 px per module");
    CHECK(panel128_qr_scale(29) == 3, "a v3 t-address at 3");
    CHECK(panel128_qr_scale(57) == 2, "v10 still at 2");
    int scanned = 0;
    for (size_t i = 0; i < ZV_KEYS_COUNT; i += 3) {
        const char *texts[2] = { ZV_KEYS[i].ua, ZV_KEYS[i].transparent_address };
        const uint8_t versions[2] = { 7, 3 }, scales[2] = { 1, 2 };
        for (int k = 0; k < 2; k++) {
            for (int inverted = 0; inverted <= 1; inverted++) {
                oled_clear();
                CHECK(oled_draw_qrcode_ex(texts[k], versions[k], scales[k], inverted) == ESP_OK, "draw");
                const QRCode *qr = oled_core_last_qr(&inv);
                CHECK(qr && inv == (bool)inverted, "the QR was not remembered");
                panel128_compose(oled_core_framebuffer(), qr, inv, img);
                if (!inverted) {
                    CHECK(scans_to(texts[k]), "%s does not scan", texts[k]);
                    scanned++;
                }
            }
        }
    }
    printf("  %d full-panel codes scanned back\n", scanned);

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
