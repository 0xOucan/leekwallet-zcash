/* The colour panel image (src/panel-color.c).
 *
 * Text is redrawn from the font at an exact integer scale, not resampled;
 * the text layer forgets a glyph the moment the frame stops showing it; roles
 * pick colours; and a QR redrawn at native scale still scans back to exactly
 * what was drawn (zbarimg, skipped when missing). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "oled.h"
#include "oled-core.h"
#include "panel-color.h"
#include "../../vectors/keys_vectors.h"

void leek_log(const char *level, const char *tag, const char *fmt, ...) { }
esp_err_t oled_refresh(void) { return ESP_OK; }

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

#define W 240
#define H 240
static uint16_t img[W * H];
static bool have_zbar;

static void render(void)
{
    PanelFrame f = {
        .frame = oled_core_framebuffer(),
        .glyphs = oled_core_glyphs(),
        .roles = oled_core_roles(),
        .picture = oled_core_panel_picture(),
        .tint = oled_core_panel_tint(),
    };
    f.qr = oled_core_last_qr(&f.qr_inverted);
    /* In uneven bands, as a transport might, to prove bands join up. */
    for (int y = 0; y < H; y += 17) {
        panel_color_render(&f, panel_color_palette(), W, H, y, (H - y < 17) ? H - y : 17,
                           img + (size_t)y * W);
    }
}

static uint16_t at(int x, int y) { return img[y * W + x]; }

static bool scans_to(const char *want)
{
    if (!have_zbar) return true;
    char path[] = "/tmp/leek-panel-color-XXXXXX";
    int fd = mkstemp(path);
    FILE *f = fdopen(fd, "wb");
    fprintf(f, "P5\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        const uint16_t c = img[i];
        fputc(((c >> 11) & 0x1F) * 255 / 31 > 127 ? 255 : 0, f);
    }
    fclose(f);
    char cmd[128], got[512] = {0};
    snprintf(cmd, sizeof cmd, "zbarimg -q --raw %s 2>/dev/null", path);
    FILE *z = popen(cmd, "r");
    size_t n = z ? fread(got, 1, sizeof got - 1, z) : 0;
    if (z) pclose(z);
    unlink(path);
    while (n && got[n - 1] == '\n') got[--n] = 0;
    if (strcmp(got, want)) printf("    scanned \"%s\"\n", got);
    return strcmp(got, want) == 0;
}

int main(void)
{
    have_zbar = system("command -v zbarimg >/dev/null 2>&1") == 0;
    printf("== test_panel_color%s\n", have_zbar ? "" : " (zbarimg missing: scans SKIPPED)");
    const PanelPalette *pal = panel_color_palette();

    /* An 'H' at column 0 of row 2 (body text): column 0 of the glyph is
       0x7F, lit rows 0-6. At 2x3 with 30-row pages and 3 rows of leading,
       glyph row r is panel rows 63+3r..65+3r, glyph column 0 panel
       columns 0-1, and nothing of it reaches column 10. */
    oled_clear();
    oled_draw_string(2, 0, "H");
    render();
    const uint16_t text = pal->fg[OLED_ROLE_TEXT];
    int wrong = 0;
    for (int y = 63; y < 63 + 21; y++) {
        for (int x = 0; x < 2; x++) wrong += at(x, y) != text;
        wrong += at(10, y) != pal->bg;
    }
    CHECK(wrong == 0, "%d pixels of the 2x3 'H' stem are wrong", wrong);
    CHECK(at(0, 62) == pal->bg && at(0, 63 + 21) == pal->bg, "the stem runs past its 7 rows");

    /* Every glyph pixel is a whole 2x3 block: across a line of mixed text,
       each block is one colour. */
    oled_clear();
    oled_draw_string(3, 0, "Send 0.004 TAZ to u1");
    render();
    int split = 0;
    for (int c = 0; c < 20 * 6; c += 6) {
        const int x0 = (c * W + 127) / 128;
        for (int gx = 0; gx < 5; gx++) {
            for (int gy = 0; gy < 7; gy++) {
                const int bx = x0 + gx * 2, by = 93 + gy * 3;
                const uint16_t v = at(bx, by);
                for (int k = 0; k < 6; k++) split += at(bx + k % 2, by + k / 2) != v;
            }
        }
    }
    CHECK(split == 0, "%d glyph blocks are not solid 2x3", split);

    /* Roles: title accent, footer dim, explicit warn, a selected row's bar. */
    oled_clear();
    oled_draw_string(0, 0, "T");
    oled_draw_string(7, 0, "F");
    oled_set_role(OLED_ROLE_WARN);
    oled_draw_string(4, 0, "W");
    oled_set_role(OLED_ROLE_AUTO);
    oled_draw_string(5, 0, "> X");
    render();
    /* 'T' top bar is glyph row 0 at columns 0..4 -> panel row 3; 'F' and
       'W' stems are glyph column 0, rows 0-6. */
    CHECK(at(4, 3) == pal->fg[OLED_ROLE_ACCENT], "the title is not accent");
    CHECK(at(0, 7 * 30 + 3) == pal->fg[OLED_ROLE_DIM], "the footer is not dim");
    CHECK(at(0, 4 * 30 + 3) == pal->fg[OLED_ROLE_WARN], "the warning is not amber");
    CHECK(at(200, 5 * 30 + 1) == pal->select_bg, "the selected row has no bar");
    CHECK(at(200, 5 * 30 + 29) == pal->select_bg, "the bar does not span the row");
    CHECK(at(0, 28) == pal->rule || at(10, 28) == pal->rule, "no rule under the title");

    /* The text layer forgets what the frame no longer shows. */
    oled_clear();
    oled_draw_string(1, 0, "ABC");
    CHECK(oled_core_glyphs()[OLED_WIDTH + 6] == 'B', "B not recorded");
    oled_draw_string(1, 3, "Z");        /* overlaps A's and B's cells */
    CHECK(oled_core_glyphs()[OLED_WIDTH + 0] == 0 && oled_core_glyphs()[OLED_WIDTH + 6] == 0,
          "an overwritten glyph was kept");
    CHECK(oled_core_glyphs()[OLED_WIDTH + 3] == 'Z', "Z not recorded");
    oled_set_pixel(4, 9, true);          /* row 1, inside Z's cell */
    CHECK(oled_core_glyphs()[OLED_WIDTH + 3] == 0, "a glyph kept after a pixel landed in it");
    oled_draw_string(2, 125, "Q");       /* clipped at the right edge */
    CHECK(oled_core_glyphs()[2 * OLED_WIDTH + 125] == 0, "a clipped glyph was recorded");
    oled_draw_string(2, 0, "K");
    oled_fill_page(2, 0x00);
    CHECK(oled_core_glyphs()[2 * OLED_WIDTH] == 0, "a glyph outlived its page fill");

    /* QR codes at native scale, four modules of quiet zone. */
    CHECK(panel_color_qr_scale(49, W, H) == 4, "v8 at 4 px per module");
    CHECK(panel_color_qr_scale(57, W, H) == 3, "v10 at 3");
    CHECK(panel_color_qr_scale(29, W, H) == 6, "v3 at 6");
    int scanned = 0;
    for (size_t i = 0; i < ZV_KEYS_COUNT; i += 3) {
        const char *texts[2] = { ZV_KEYS[i].ua, ZV_KEYS[i].transparent_address };
        const uint8_t versions[2] = { 7, 3 }, scales[2] = { 1, 2 };
        for (int k = 0; k < 2; k++) {
            oled_clear();
            CHECK(oled_draw_qrcode_ex(texts[k], versions[k], scales[k], false) == ESP_OK, "draw");
            render();
            CHECK(scans_to(texts[k]), "%s does not scan", texts[k]);
            scanned++;
        }
    }
    printf("  %d native-scale codes scanned back\n", scanned);

    /* The splash picture wins over everything until a clear. */
    static uint8_t picture[128 * 16];
    memset(picture, 0xFF, sizeof picture);
    oled_show_panel_picture(picture);
    render();
    CHECK(at(0, 0) == pal->picture && at(239, 239) == pal->picture, "the picture is not full-panel");
    CHECK(pal->picture == PANEL_RGB565(57, 255, 122), "the splash is not the website's GREEN");

    /* Its colour layer: one code per band of 32 rows, the top band also
       unlit to show the halo. A lit pixel takes its code's colour, an unlit
       one is the background unless the code is the halo. */
    static uint8_t tint[OLED_TINT_BYTES];
    for (int y = 0; y < 128; y++) memset(tint + y * OLED_TINT_ROW, 0x55 * (y / 32), OLED_TINT_ROW);
    for (int x = 0; x < 128; x++) picture[x] = 0xFE;   /* row 0 unlit */
    oled_show_panel_tint(tint);
    CHECK(oled_core_panel_tint() == tint, "the tint was not kept");
    render();
    CHECK(at(0, 0) == pal->bg, "an unlit, untinted pixel is not the background");
    CHECK(at(0, 10) == pal->picture, "code 0 is not the picture colour");
    CHECK(at(100, 70) == pal->picture_key, "code 1 is not the key");
    CHECK(at(100, 130) == pal->picture_spark, "code 2 is not a sparkle");
    CHECK(at(100, 200) == pal->picture, "a lit halo pixel is not the picture colour");
    for (int x = 0; x < 128; x++) picture[15 * 128 + x] = 0x7F;   /* row 127 unlit */
    render();
    CHECK(at(10, 239) == pal->picture_halo, "an unlit halo pixel is not lit dim");
    CHECK(pal->picture_halo != pal->bg && pal->picture_halo != pal->picture_spark, "the halo has no colour of its own");
    oled_clear();
    CHECK(oled_core_panel_tint() == NULL, "the tint outlived a clear");
    oled_show_panel_tint(tint);
    CHECK(oled_core_panel_tint() == NULL, "a tint without a picture");

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
