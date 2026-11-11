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
#include "camera.h"
#include "qrcode.h"
#include "ur-encoder.h"

void pin__reset_static_state_for_test(void);
void ui__reset_static_state_for_test(void);
void ui__service_wrong_network_for_test(void);
void fake_input_reset(void);
void ui__service_scan_for_test(void);
void ui__scan_feed_for_test(const char *ur);

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
esp_err_t oled_set_backlight(uint8_t pct) { (void)pct; return ESP_OK; }

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
        .tint = oled_core_panel_tint(),
    };
    f.qr = oled_core_last_qr(&f.qr_inverted);
    const OledPanelGray g = oled_core_panel_gray();
    f.gray = g.px;
    f.gray_x = g.x; f.gray_y = g.y; f.gray_w = g.w; f.gray_h = g.h;
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

/* A synthetic camera frame: a dim room with a lit phone held at an angle's
 * worth of offset, a QR on its screen, a little sensor noise. Only for
 * looking at; the suites test the arithmetic. */
static uint8_t cam[320 * 240];
static void camera_scene(int dim)
{
    uint32_t seed = 12345;
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) {
            seed = seed * 1103515245u + 12345u;
            const int room = 0x18 + x / 16 + y / 20 + (int)((seed >> 16) & 7);
            cam[y * 320 + x] = (uint8_t)(room / dim);
        }
    static uint8_t mod[200];
    QRCode qr;
    qrcode_initText(&qr, mod, 3, 0, "UR:LEEK-PCZT/1-9/LPADAXCFAXHL");
    const int s = 4, side = qr.size * s, ox = 120, oy = 50;
    for (int y = oy - 24; y < oy + side + 30; y++)
        for (int x = ox - 18; x < ox + side + 18; x++) cam[y * 320 + x] = (uint8_t)(0xC8 / dim);
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++)
            if (qrcode_getModule(&qr, (uint8_t)(x / s), (uint8_t)(y / s)))
                cam[(oy + y) * 320 + ox + x] = (uint8_t)(0x30 / dim);
}

#if UI_RENDER_SE
#include "fake_se_store.h"

/* The PIN, as the knob types it: DOWN steps the digit up from 0, OK takes
 * it, and once four are in, UP from 0 wraps to the OK option. */
static void type_pin(const char *pin)
{
    for (const char *p = pin; *p; p++) {
        for (int i = 0; i < *p - '0'; i++) press(BUTTON_DOWN);
        press(BUTTON_ACCEPT);
    }
    press(BUTTON_UP);
}

/* Settings > Secure element, as the esp32s3cam-color-se image draws it. */
static int render_se(void)
{
    boot(true);
    fake_se_reset();
    go(SCREEN_SE_BACKUP);
    snap("se-01-menu");
    press(BUTTON_ACCEPT);                 /* Info */
    type_pin("1234");
    snap("se-02-pin");
    press(BUTTON_ACCEPT);
    snap("se-03-info-empty");
    press(BUTTON_ACCEPT);
    press(BUTTON_DOWN);                   /* Copy seed to SE */
    press(BUTTON_ACCEPT);
    type_pin("1234");
    press(BUTTON_ACCEPT);
    snap("se-04-copied");
    press(BUTTON_ACCEPT);
    press(BUTTON_DOWN);                   /* Compare SE copy */
    press(BUTTON_ACCEPT);
    type_pin("1234");
    press(BUTTON_ACCEPT);
    snap("se-05-compare-match");
    press(BUTTON_ACCEPT);
    press(BUTTON_DOWN);
    press(BUTTON_DOWN);                   /* SE copy -> vault */
    press(BUTTON_ACCEPT);
    type_pin("1234");
    press(BUTTON_ACCEPT);
    snap("se-06-restore-vault-noop");
    press(BUTTON_ACCEPT);
    press(BUTTON_DOWN);                   /* Erase SE copy */
    press(BUTTON_ACCEPT);
    type_pin("1234");
    press(BUTTON_ACCEPT);
    snap("se-07-erase-confirm");
    press(BUTTON_ACCEPT);
    snap("se-08-erased");
    press(BUTTON_ACCEPT);
    for (int i = 0; i < 8; i++) press(BUTTON_DOWN);
    snap("se-09-menu-end");
    return 0;
}
#endif

int main(int argc, char **argv)
{
    if (argc > 1) out_dir = argv[1];
    printf("== render_screens -> %s\n", out_dir);
#if UI_RENDER_SE
    return render_se();
#endif

    boot(false);
    go(SCREEN_BOOT);
    snap("01-splash");
    /* The key risen and glowing, the sparkles out: 1.2 s in, frame 11
       (the website at t = 2400 ms). */
    fake_clock_advance_us(1200 * 1000);
    ui_render();
    snap("01b-splash-key");
    /* The held frame, text and prompt under the logo. */
    fake_clock_advance_us(600 * 1000);
    ui_render();
    snap("01c-splash-text");
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
    ui_notify_app_outdated();
    ui__service_wrong_network_for_test();
    ui_render();
    snap("12b-update-your-app");
    press(BUTTON_ACCEPT);

    go(SCREEN_WIPE_CONFIRM);
    snap("13-wipe");
    press(BUTTON_CANCEL);

    /* zGetAccount's question, as a companion paired over USB asks it. */
    ui_request_share_view_key(0, ZS_NET_MAIN, "482913", false);
    go(SCREEN_SHARE_VIEW_KEY);
    snap("14-share-view-key");
    press(BUTTON_CANCEL);
    ui_sign_clear();

    /* zGetStorageKey's question: a short app identifier, then the longest. */
    ui_request_storage_key("xyz.leekwallet.zcash", 0, ZS_NET_MAIN, "482913", false);
    go(SCREEN_STORAGE_KEY);
    snap("15-storage-key");
    press(BUTTON_CANCEL);
    ui_sign_clear();
    ui_request_storage_key("com.example.wallet.desktop-edition.beta.channel.nightly-build", 3,
                           ZS_NET_TEST, "482913", true);
    go(SCREEN_STORAGE_KEY);
    snap("16-storage-key-long");
    press(BUTTON_CANCEL);
    ui_sign_clear();

    /* The viewfinder: a QR on a phone in a dim room, auto-levelled. */
    camera_scene(1);
    camera__set_frame_for_test(cam, 320, 240);
    camera__set_located_for_test(4, 0);
    go(SCREEN_SCAN);
    ui__service_scan_for_test();
    fake_clock_advance_us(1000 * 1000);
    ui__service_scan_for_test();
    ui_render();
    snap("17-scan");
    /* An animated UR part way in: the progress bar. */
    static uint8_t cbor[1200];
    for (size_t i = 0; i < sizeof cbor; i++) cbor[i] = (uint8_t)(i * 7);
    cbor[0] = 0x59; cbor[1] = (sizeof cbor - 3) >> 8; cbor[2] = (sizeof cbor - 3) & 0xFF;
    static UrEncoder enc;
    char part[600];
    ur_encoder_init(&enc, "leek-pczt", cbor, sizeof cbor, 200);
    for (int i = 0; i < 3; i++) {
        ur_encoder_next_part(&enc, part, sizeof part);
        ui__scan_feed_for_test(part);
    }
    ui_render();
    snap("18-scan-progress");
    press(BUTTON_CANCEL);
    /* Too dark: the same scene at a quarter the light, still visible. */
    camera_scene(5);
    camera__set_located_for_test(0, 0);
    go(SCREEN_SCAN);
    ui__service_scan_for_test();
    ui_render();
    snap("19-scan-dark");
    press(BUTTON_CANCEL);
    /* And as the decoder sees it: Settings > Preview True. */
    camera_set_preview_true_exposure(true);
    go(SCREEN_SCAN);
    ui__service_scan_for_test();
    ui_render();
    snap("20-scan-true-exposure");
    press(BUTTON_CANCEL);
    camera_set_preview_true_exposure(false);
    camera__set_frame_for_test(NULL, 0, 0);

    /* Settings > Screen, being turned. */
    go(SCREEN_SETTINGS);
    for (int i = 0; i < 40; i++) {
        press(BUTTON_DOWN);
        /* Stop on the Screen line: row 2, 4 or 6 starting with "> S". */
        bool found = false;
        for (int r = 2; r <= 6; r += 2) {
            const uint8_t *g = oled_core_glyphs() + r * 128;
            if (g[0] == '>' && g[12] == 'S' && g[18] == 'c') found = true;
        }
        if (found) break;
    }
    press(BUTTON_ACCEPT);
    press(BUTTON_DOWN);
    press(BUTTON_DOWN);
    press(BUTTON_DOWN);
    snap("21-settings-screen");
    press(BUTTON_CANCEL);
    return 0;
}
