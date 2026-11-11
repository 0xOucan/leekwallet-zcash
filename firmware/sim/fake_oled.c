/**
 * src/oled.h implemented against RAM instead of I2C. See fake_oled.h.
 */

#include "fake_oled.h"
#include "oled.h"
#include "backlight-level.h"

#include <stdio.h>
#include <string.h>

static char    text[FAKE_OLED_ROWS][FAKE_OLED_COLS + 1];
/* The role each text cell was drawn in, as oled-core keeps per column. */
static uint8_t text_role[FAKE_OLED_ROWS][FAKE_OLED_COLS];
static uint8_t current_role;
static uint8_t fb[OLED_PAGES][OLED_WIDTH];   /* SSD1306 order: one byte = 8 rows */
static char    qr_data[400];   /* a version 10 frame is 395 characters */
static uint8_t qr_version, qr_scale;
static int     flush_count;
static uint8_t contrast = 0;
static uint8_t panel_rows = OLED_HEIGHT;
static const uint8_t *panel_picture;
static int     picture_flushes;
static const uint8_t *panel_gray;
static uint16_t panel_gray_x, panel_gray_y, panel_gray_w, panel_gray_h;
static uint16_t backlight = BL_DUTY_MAX;
static int     backlight_calls;
static char    qr_caption[OLED_QR_CAPTION_MAX + 1];
static bool    qr_fill;

static uint8_t cursor_page;
static uint8_t cursor_col;

/* Trimmed copies handed out by fake_oled_row(). Kept per row so a test can
 * hold two of them in one printf without them aliasing. */
static char trimmed[FAKE_OLED_ROWS][FAKE_OLED_COLS + 1];

static void blank_text(void)
{
    memset(text_role, 0, sizeof text_role);
    current_role = 0;
    for (int p = 0; p < FAKE_OLED_ROWS; p++) {
        memset(text[p], ' ', FAKE_OLED_COLS);
        text[p][FAKE_OLED_COLS] = '\0';
    }
}

void fake_oled_reset(void)
{
    blank_text();
    memset(fb, 0, sizeof(fb));
    qr_data[0] = '\0';
    qr_version = 0;
    qr_scale = 0;
    flush_count = 0;
    contrast = 0;
    cursor_page = 0;
    cursor_col = 0;
    panel_rows = OLED_HEIGHT;
    panel_picture = NULL;
    picture_flushes = 0;
    panel_gray = NULL;
    backlight = BL_DUTY_MAX;
    backlight_calls = 0;
    qr_caption[0] = '\0';
    qr_fill = false;
}

void fake_oled_set_panel_rows(uint8_t rows) { panel_rows = rows; }
const uint8_t *fake_oled_panel_picture(void) { return panel_picture; }
int fake_oled_picture_flush_count(void) { return picture_flushes; }

const uint8_t *fake_oled_panel_gray(uint16_t *x, uint16_t *y, uint16_t *w, uint16_t *h)
{
    if (x) *x = panel_gray_x;
    if (y) *y = panel_gray_y;
    if (w) *w = panel_gray_w;
    if (h) *h = panel_gray_h;
    return panel_gray;
}
uint16_t fake_oled_backlight_duty(void) { return backlight; }
const char *fake_oled_qr_caption(void) { return qr_caption; }
bool fake_oled_qr_fill(void) { return qr_fill; }
void oled_set_qr_fill(bool fill) { qr_fill = fill; }

void oled_set_qr_caption(const char *text)
{
    snprintf(qr_caption, sizeof qr_caption, "%s", text ? text : "");
}
int fake_oled_backlight_calls(void) { return backlight_calls; }

void oled_show_panel_gray(const uint8_t *px, uint16_t x, uint16_t y,
                          uint16_t w, uint16_t h)
{
    panel_gray = px;
    panel_gray_x = x; panel_gray_y = y; panel_gray_w = w; panel_gray_h = h;
}

esp_err_t oled_set_backlight_duty(uint16_t duty)
{
    if (duty < 1) duty = 1;
    if (duty > BL_DUTY_MAX) duty = BL_DUTY_MAX;
    backlight = duty;
    backlight_calls++;
    return ESP_OK;
}

/* ------------------------------------------------------------ inspection */

const char *fake_oled_row(int page)
{
    if (page < 0 || page >= FAKE_OLED_ROWS) {
        return "";
    }
    memcpy(trimmed[page], text[page], FAKE_OLED_COLS + 1);
    for (int i = FAKE_OLED_COLS - 1; i >= 0 && trimmed[page][i] == ' '; i--) {
        trimmed[page][i] = '\0';
    }
    return trimmed[page];
}

bool fake_oled_row_contains(int page, const char *needle)
{
    return strstr(fake_oled_row(page), needle) != NULL;
}

int fake_oled_find_row(const char *needle)
{
    for (int p = 0; p < FAKE_OLED_ROWS; p++) {
        if (fake_oled_row_contains(p, needle)) {
            return p;
        }
    }
    return -1;
}

bool fake_oled_contains(const char *needle)
{
    return fake_oled_find_row(needle) >= 0;
}

const char *fake_oled_qr_data(void) { return qr_data; }
int         fake_oled_flush_count(void) { return flush_count; }
uint8_t     fake_oled_contrast(void) { return contrast; }

void fake_oled_dump(void)
{
    printf("    +---------------------+\n");
    for (int p = 0; p < FAKE_OLED_ROWS; p++) {
        printf("  %d |%-*s|\n", p, FAKE_OLED_COLS, fake_oled_row(p));
    }
    printf("    +---------------------+\n");
}

bool fake_oled_pixel(int x, int y)
{
    if (x < 0 || y < 0 || x >= OLED_WIDTH || y >= OLED_HEIGHT) {
        return false;
    }
    return (fb[y / 8][x] >> (y % 8)) & 1;
}

void fake_oled_dump_pixels(void)
{
    for (int y = 0; y < OLED_HEIGHT; y++) {
        for (int x = 0; x < OLED_WIDTH; x++) {
            putchar((fb[y / 8][x] >> (y % 8)) & 1 ? '#' : '.');
        }
        putchar('\n');
    }
}

/* ------------------------------------------------------------ oled.h API */

esp_err_t oled_i2c_init(void) { return ESP_OK; }

esp_err_t oled_init(void)
{
    fake_oled_reset();
    return ESP_OK;
}

esp_err_t oled_clear(void)
{
    blank_text();
    memset(fb, 0, sizeof(fb));
    cursor_page = 0;
    cursor_col = 0;
    panel_picture = NULL;
    panel_gray = NULL;
    qr_caption[0] = '\0';
    qr_fill = false;
    return ESP_OK;
}

esp_err_t oled_flush(void)
{
    flush_count++;
    if (panel_picture) picture_flushes++;
    return ESP_OK;
}

bool oled_present(void) { return true; }
uint8_t oled_panel_rows(void) { return panel_rows; }
void oled_show_panel_picture(const uint8_t *picture) { panel_picture = picture; }

esp_err_t oled_clear_panel_now(void) { return oled_clear(); }

esp_err_t oled_set_fast_refresh(bool fast)
{
    (void)fast;
    return ESP_OK;
}

/* Roles only colour a colour panel; the grid keeps them per cell so a test
   can ask what colour a reading was drawn in. */
void oled_set_role(oled_role_t role)
{
    current_role = (role < OLED_ROLE_COUNT) ? (uint8_t)role : OLED_ROLE_AUTO;
}

oled_role_t fake_oled_role_at(int page, int cell)
{
    if (page < 0 || page >= FAKE_OLED_ROWS || cell < 0 || cell >= FAKE_OLED_COLS) {
        return OLED_ROLE_AUTO;
    }
    return (oled_role_t)text_role[page][cell];
}

esp_err_t oled_set_contrast(uint8_t level)
{
    contrast = level;
    return ESP_OK;
}

esp_err_t oled_clear_page(uint8_t page)
{
    if (page >= OLED_PAGES) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(text[page], ' ', FAKE_OLED_COLS);
    memset(fb[page], 0, OLED_WIDTH);
    return ESP_OK;
}

esp_err_t oled_set_cursor(uint8_t page, uint8_t col)
{
    if (page >= OLED_PAGES || col >= OLED_WIDTH) {
        return ESP_ERR_INVALID_ARG;
    }
    cursor_page = page;
    cursor_col = col;
    return ESP_OK;
}

esp_err_t oled_draw_char(char c)
{
    if (cursor_page < FAKE_OLED_ROWS) {
        int cell = cursor_col / 6;
        if (cell < FAKE_OLED_COLS) {
            text[cursor_page][cell] = c;
            text_role[cursor_page][cell] = current_role;
        }
    }
    /* The real driver wraps by advancing 6 px and stopping at the edge. */
    if (cursor_col + 6 < OLED_WIDTH) {
        cursor_col += 6;
    }
    return ESP_OK;
}

esp_err_t oled_draw_string(uint8_t page, uint8_t col, const char *str)
{
    if (page >= OLED_PAGES || !str) {
        return ESP_ERR_INVALID_ARG;
    }
    int cell = col / 6;
    for (; *str && cell < FAKE_OLED_COLS; str++, cell++) {
        text[page][cell] = *str;
        if (page < FAKE_OLED_ROWS) { text_role[page][cell] = current_role; }
    }
    return ESP_OK;
}

esp_err_t oled_draw_string_centered(uint8_t page, const char *str)
{
    if (!str) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Same arithmetic as the driver, so a string that would be clipped on the
     * panel is clipped here too. */
    int len = (int)strlen(str);
    int col = (OLED_WIDTH - len * 6) / 2;
    if (col < 0) {
        col = 0;
    }
    return oled_draw_string(page, (uint8_t)col, str);
}

esp_err_t oled_invert_page(uint8_t page, bool invert)
{
    if (page >= OLED_PAGES) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int x = 0; x < OLED_WIDTH; x++) {
        fb[page][x] = invert ? (uint8_t)~fb[page][x] : fb[page][x];
    }
    return ESP_OK;
}

esp_err_t oled_fill_page(uint8_t page, uint8_t pattern)
{
    if (page >= OLED_PAGES) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(fb[page], pattern, OLED_WIDTH);
    return ESP_OK;
}

void oled_blit_page(uint8_t page, const uint8_t *cols, size_t len)
{
    if (cols == NULL || page >= OLED_PAGES) {
        return;
    }
    if (len > OLED_WIDTH) {
        len = OLED_WIDTH;
    }
    memcpy(&fb[page][0], cols, len);
}

esp_err_t oled_draw_raw(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len && cursor_col < OLED_WIDTH; i++) {
        fb[cursor_page][cursor_col++] = data[i];
    }
    return ESP_OK;
}

esp_err_t oled_set_pixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (on) {
        fb[y / 8][x] |= (uint8_t)(1u << (y % 8));
    } else {
        fb[y / 8][x] &= (uint8_t)~(1u << (y % 8));
    }
    return ESP_OK;
}

/* The QR encoder itself lives in qrcode.c and is tested on its own terms; what
 * the UI layer can get wrong is *which string* it asks for. Record that. */
esp_err_t oled_draw_qrcode(const char *data)
{
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(qr_data, sizeof(qr_data), "%s", data);
    return ESP_OK;
}

/* The return path records the string and the mode it was asked for. Whether
 * that pair fits is decided by the real rule, so a screen cannot pass here by
 * asking for a version the panel cannot show. */
bool oled_qr_fits(uint8_t version, uint8_t scale)
{
    if (version < 1 || version > OLED_QR_MAX_VERSION || scale < 1 || scale > 2) {
        return false;
    }
    return (unsigned)(version * 4 + 17) * scale + 4 <= OLED_HEIGHT;
}

static bool qr_inverted;

esp_err_t oled_draw_qrcode_ex(const char *data, uint8_t version, uint8_t scale,
                              bool inverted)
{
    if (!data || !oled_qr_fits(version, scale)) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(qr_data, sizeof(qr_data), "%s", data);
    qr_version = version;
    qr_scale = scale;
    qr_inverted = inverted;
    return ESP_OK;
}

esp_err_t oled_draw_qrcode_at(const char *data, uint8_t version, uint8_t scale)
{
    return oled_draw_qrcode_ex(data, version, scale, false);
}

bool fake_oled_qr_inverted(void) { return qr_inverted; }

uint8_t fake_oled_qr_version(void) { return qr_version; }
uint8_t fake_oled_qr_scale(void)   { return qr_scale; }

esp_err_t oled_refresh(void) { return oled_flush(); }
