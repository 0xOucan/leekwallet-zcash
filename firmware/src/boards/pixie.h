/**
 * Firefly Pixie rev.5 (ESP32-C3): the pin map. Included by board.h only.
 *
 * Source: `firefly-hollows/src/config.h`, `BOARD_REV == 5`. Earlier revisions
 * differ - rev.2 and rev.4 use other button pins and rev.4 has one LED rather
 * than four - so the revision is named here rather than assumed.
 *
 * Everything on this board is soldered down, so there is exactly one kit:
 * the ST7789 and four buttons. A build that asks for anything else is refused
 * in board.h.
 */

#ifndef LEEK_BOARDS_PIXIE_H
#define LEEK_BOARDS_PIXIE_H

#define BOARD_NAME              "Firefly Pixie"
#define BOARD_MODEL             "LeekWallet-Pixie"

#define BOARD_DEFAULT_DISPLAY   LEEK_DISPLAY_ST7789
#define BOARD_DEFAULT_INPUT     LEEK_INPUT_BUTTONS4
#define BOARD_SUPPORTS_DISPLAY(d)  ((d) == LEEK_DISPLAY_ST7789)
#define BOARD_SUPPORTS_INPUT(i)    ((i) == LEEK_INPUT_BUTTONS4)

/*
 * Buttons, in PHYSICAL order - SW1..SW4 left to right - not in Firefly's
 * semantic order.
 *
 * Firefly's own config assigns meanings to its four keys: Button 1 is Cancel,
 * Button 2 is OK, Button 3 North and Button 4 South. Mapping those meanings
 * straight through looked like the respectful thing to do and was wrong in
 * use: it puts Up and Down on the third and fourth keys, so the same gesture
 * is in a different place depending on which board you picked up. Reported
 * from a Pixie after the first wallet was created on one.
 *
 * The reference board is UP, DOWN, BACK, OK across the row, and that is what a
 * user's hand learns. The Pixie matches it. Firefly's labels are theirs and
 * still correct for their firmware; this is a different application on the same
 * hardware, and consistency between OUR two boards is worth more than agreement
 * with somebody else's key names.
 *
 * Active low with internal pull-ups, same as the reference board.
 */
#define PIN_BUTTON_UP           GPIO_NUM_10   /* SW1 - Firefly calls it Button 1 */
#define PIN_BUTTON_DOWN         GPIO_NUM_8    /* SW2 - Firefly's Button 2 */
#define PIN_BUTTON_CANCEL       GPIO_NUM_3    /* SW3 - Firefly's Button 3 */
#define PIN_BUTTON_ACCEPT       GPIO_NUM_2    /* SW4 - Firefly's Button 4 */

/*
 * Display: ST7789 on SPI2 at the C3's IOMUX pins (CLK 6, MOSI 7), which is
 * what firefly-display used. CS is tied to ground from rev.3 onward, which is
 * why the bus runs without it. The panel's own settings - orientation, gamma,
 * pixel byte order - are the Pixie profile in oled-st7789.c.
 */
#define PIN_DISPLAY_SCLK        GPIO_NUM_6
#define PIN_DISPLAY_MOSI        GPIO_NUM_7
#define PIN_DISPLAY_CS          GPIO_NUM_NC
#define PIN_DISPLAY_DC          GPIO_NUM_4
#define PIN_DISPLAY_RESET       GPIO_NUM_5
#define ST7789_PROFILE_PIXIE    1

/* Four addressable LEDs, one beside each button. */
#define PIN_PIXELS              GPIO_NUM_9
#define PIXEL_COUNT             4

/*
 * No I2C bus is broken out. Pointed at nothing so a stray initialisation fails
 * loudly rather than quietly driving GPIO 8 and 9 - Button 2 and the LED
 * string, and GPIO9 is also the C3's boot strapping pin.
 */
#define PIN_I2C_SDA             GPIO_NUM_NC
#define PIN_I2C_SCL             GPIO_NUM_NC

#define LEEK_HAS_CAMERA         0
#define LEEK_HAS_ZCASH          0   /* zsign is built for Xtensa; this is a RISC-V C3 */
#define LEEK_HAS_SDCARD         0
#define LEEK_HAS_BLE            1
#define LEEK_HAS_USB            0   /* USB Serial/JTAG only; no wallet transport */
#define LEEK_VAULT_ON_SD        0

#endif /* LEEK_BOARDS_PIXIE_H */
