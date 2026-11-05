/**
 * The ESP32-S3 reference board: the pin map. Included by board.h only.
 *
 * An ESP32-S3 dev board on a breadboard with the four-button module and a
 * 128x64 SSD1306. Its kit is the one this firmware was written on, and the
 * only one wired for it.
 */

/* Read twice by board.h: first the identity and the kit it can carry, then,
   with the components chosen, the pins. */
#ifndef LEEK_BOARD_PINS

#define BOARD_NAME              "ESP32-S3"
#define BOARD_MODEL             "LeekWallet-S3"

#define BOARD_DEFAULT_DISPLAY   LEEK_DISPLAY_SSD1306
#define BOARD_DEFAULT_INPUT     LEEK_INPUT_BUTTONS4
#define BOARD_SUPPORTS_DISPLAY(d)  ((d) == LEEK_DISPLAY_SSD1306 || (d) == LEEK_DISPLAY_SH1107)
#define BOARD_SUPPORTS_INPUT(i)    ((i) == LEEK_INPUT_BUTTONS4)

#else /* LEEK_BOARD_PINS */

/* GPIO4 was unusable on the reference board - stuck low - which is why K1 is
   on GPIO10 and not where a datasheet would put it. */
#define PIN_BUTTON_UP           GPIO_NUM_10   /* K1 */
#define PIN_BUTTON_DOWN         GPIO_NUM_5    /* K2 */
#define PIN_BUTTON_CANCEL       GPIO_NUM_6    /* K3 */
#define PIN_BUTTON_ACCEPT       GPIO_NUM_7    /* K4 */

/* The I2C bus, for the panel at 0x3C. */
#define PIN_I2C_SDA             GPIO_NUM_8
#define PIN_I2C_SCL             GPIO_NUM_9

/* No addressable LEDs on the reference board. Defined anyway, for the same
   reason the capability flags are: a board that omits a macro turns a typo
   elsewhere into a silent zero. */
#define PIN_PIXELS              GPIO_NUM_NC
#define PIXEL_COUNT             0

/* microSD for the SD storage mode (roadmap 1.4), wired like the CAM board's
   slot so one card and one firmware path serve both: 1-bit SDMMC, CMD 38,
   CLK 39, D0 40 (free on the N16R8: octal PSRAM takes 35-37). Use a passive
   microSD breakout with pull-ups; the common "SPI module" with a 74LVC125
   buffer cannot carry SD mode's bidirectional CMD line. No card: the SD
   screens say so and nothing else changes. */
#define PIN_SD_CMD              GPIO_NUM_38
#define PIN_SD_CLK              GPIO_NUM_39
#define PIN_SD_D0               GPIO_NUM_40
#define BOARD_SD_BUS_WIDTH      1

#define LEEK_HAS_CAMERA         0
#define LEEK_HAS_ZCASH          1
#define LEEK_HAS_SDCARD         1
#define LEEK_HAS_BLE            1
#define LEEK_HAS_USB            1
#define LEEK_VAULT_ON_SD        0

#endif /* LEEK_BOARD_PINS */
