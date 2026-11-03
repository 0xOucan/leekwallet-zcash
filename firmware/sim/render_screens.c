/**
 * Screens of the real UI on the colour panel, as PNG files.
 *
 * The real ui.c draws into the real oled-core.c; this file is the panel. Each
 * flush is rendered by panel-color.c exactly as oled-st7789.c renders it on
 * the board (minus the byte swap for the bus), and, beside it, by panel128.c
 * as the SH1107 kit would show the same frame. So a layout or palette change
 * can be looked at - and compared with the shipped OLED - without a board.
 *
 *   make screens     # writes build/screens/<name>.png
 *
 * Built for the S3 CAM colour kit (knob hints, 240-row panel). Asserts
 * nothing; test_panel_color is the suite.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_stubs.h"
#include "fake_nvs.h"
#include "fake_wallet.h"

#include "oled.h"
#include "oled-core.h"
#include "panel-color.h"
#include "panel128.h"
#include "pin.h"
#include "ui.h"
#include "airgap.h"
#include "zcash-review.h"
#include "leek-wallet.h"
#include "button.h"

void pin__reset_static_state_for_test(void);
void ui__reset_static_state_for_test(void);
void ui__service_wrong_network_for_test(void);
void fake_input_reset(void);

#define W 240
#define H 240

static const char *out_dir = ".";

/* ------------------------------------------------- the panel, on the host */

esp_err_t oled_i2c_init(void) { return ESP_OK; }
esp_err_t oled_init(void) { return ESP_OK; }
esp_err_t oled_flush(void) { return ESP_OK; }
esp_err_t oled_refresh(void) { return ESP_OK; }
esp_err_t oled_set_fast_refresh(bool fast) { (void)fast; return ESP_OK; }
esp_err_t oled_set_contrast(uint8_t level) { (void)level; return ESP_OK; }
esp_err_t oled_clear_panel_now(void) { return oled_clear(); }
esp_err_t oled_draw_raw(const uint8_t *d, size_t n) { (void)d; (void)n; return ESP_FAIL; }
bool oled_present(void) { return true; }
uint8_t oled_panel_rows(void) { return H; }

/* One image: the colour panel on the left; the SH1107's 128x128 at 2x
 * (white on black) on the right, both on a grey card. */
static void snap(const char *name)
{
    static uint16_t colour[W * H];
    static uint8_t mono[PANEL128_BYTES];
    PanelFrame f = {
        .frame = oled_core_framebuffer(),
        .glyphs = oled_core_glyphs(),
        .roles = oled_core_roles(),
        .picture = oled_core_panel_picture(),
    };
    f.qr = oled_core_last_qr(&f.qr_inverted);
    panel_color_render(&f, panel_color_palette(), W, H, 0, H, colour);
    panel128_compose(oled_core_framebuffer(), oled_core_panel_picture(), f.qr,
                     f.qr_inverted, mono);

    enum { M = 16, IW = M + W + M + 256 + M, IH = M + 256 + M };
    char ppm[512], png[512], cmd[1200];
    snprintf(ppm, sizeof ppm, "%s/%s.ppm", out_dir, name);
    snprintf(png, sizeof png, "%s/%s.png", out_dir, name);
    FILE *o = fopen(ppm, "wb");
    if (!o) { perror(ppm); exit(1); }
    fprintf(o, "P6\n%d %d\n255\n", IW, IH);
    for (int y = 0; y < IH; y++) {
        for (int x = 0; x < IW; x++) {
            uint8_t r = 0x50, g = 0x50, b = 0x50;
            const int cx = x - M, cy = y - M - 8;
            const int mx = (x - (M + W + M)) / 2, my = (y - M) / 2;
            if (cx >= 0 && cx < W && cy >= 0 && cy < H) {
                const uint16_t c = colour[cy * W + cx];
                r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
                g = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
                b = (uint8_t)((c & 0x1F) * 255 / 31);
            } else if (x >= M + W + M && mx < 128 && my >= 0 && my < 128) {
                const bool on = (mono[(my / 8) * 128 + mx] >> (my % 8)) & 1;
                r = g = b = on ? 0xE0 : 0x00;
                if (on) b = 0xFF;
            }
            fputc(r, o); fputc(g, o); fputc(b, o);
        }
    }
    fclose(o);
    snprintf(cmd, sizeof cmd, "convert '%s' '%s' && rm -f '%s'", ppm, png, ppm);
    if (system(cmd) != 0) {
        fprintf(stderr, "convert failed; left %s\n", ppm);
    } else {
        printf("  %s\n", png);
    }
}

/* ---------------------------------------------------------------- driving */

static void press(button_id_t b) { ui_handle_button(b); ui_render(); }
static void go(screen_id_t s) { ui_set_screen(s); ui_render(); }

static void boot(bool with_seed)
{
    fake_nvs_reset();
    fake_clock_reset();
    fake_input_reset();
    fake_wallet_reset();
    pin__reset_static_state_for_test();
    ui__reset_static_state_for_test();
    pin_init();
    wallet_init();
    ui_init();
    if (with_seed) {
        pin_set("1234");
        pin_verify("1234");
        fake_wallet_preload("legal winner thank year wave sausage worth useful "
                            "legal winner thank yellow");
    }
    ui_render();
}

int main(int argc, char **argv)
{
    if (argc > 1) out_dir = argv[1];
    printf("== render_screens -> %s\n", out_dir);

    boot(false);
    go(SCREEN_BOOT);
    snap("01-splash");
    go(SCREEN_PIN_SETUP);
    snap("02-pin-setup");

    boot(true);
    go(SCREEN_MAIN_MENU);
    snap("03-home");
    press(BUTTON_DOWN);
    snap("04-home-scrolled");
    go(SCREEN_SETTINGS);
    snap("05-settings");
    go(SCREEN_WALLET_INFO);
    snap("06-wallet-info");

    /* A Zcash review with a public (transparent) output, which carries the
       '!' warning rows. */
    static zs_summary s;
    memset(&s, 0, sizeof s);
    s.branch_id = 0x37a5165b;
    s.fee = 10000;
    s.n_spends = 1;
    s.n_outputs = 2;
    s.warnings = ZS_WARN_TRANSPARENT_OUT;
    s.outputs[0].pool = ZS_POOL_TRANSPARENT;
    s.outputs[0].kind = ZS_KIND_EXTERNAL;
    s.outputs[0].value = 400000;
    strcpy(s.outputs[0].address, "tmBsTi2xWTjUdEXnuTceL7fecEQKeWaPDJd");
    s.outputs[1].pool = ZS_POOL_ORCHARD;
    s.outputs[1].kind = ZS_KIND_CHANGE;
    s.outputs[1].value = 590000;
    ui_request_zsign_review(&s, ZS_NET_TEST);
    go(SCREEN_ZSIGN_REVIEW);
    snap("07-review-send");
    const int pages = zreview_page_count(&s, ZS_NET_TEST);
    for (int i = 1; i < pages; i++) press(BUTTON_DOWN);
    snap("08-review-sign");
    press(BUTTON_CANCEL);
    ui_sign_clear();

    /* The QR reply: a 108-byte .zsig, one transparent input's worth. */
    uint8_t zsig[108];
    for (size_t i = 0; i < sizeof zsig; i++) zsig[i] = (uint8_t)(i * 37 + 11);
    if (ui_show_ur(AIRGAP_UR_ZSIG, zsig, sizeof zsig, SCREEN_MAIN_MENU)) {
        ui_render();
        snap("09-qr-reply-still");
    } else {
        fprintf(stderr, "ui_show_ur refused the reply\n");
    }
    go(SCREEN_MAIN_MENU);

    go(SCREEN_SHOW_ADDRESS);
    snap("10-receive");
    press(BUTTON_DOWN);
    snap("11-receive-2");

    go(SCREEN_MAIN_MENU);
    ui_notify_wrong_network(1);
    ui__service_wrong_network_for_test();
    ui_render();
    snap("12-wrong-network");
    press(BUTTON_ACCEPT);

    go(SCREEN_WIPE_CONFIRM);
    snap("13-wipe");
    return 0;
}
