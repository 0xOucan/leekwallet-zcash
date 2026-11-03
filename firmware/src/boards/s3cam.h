/**
 * ESP32-S3-N16R8 CAM + OV5640: the pin map. Included by board.h only.
 *
 * Every pin below comes from docs/BOARD-S3CAM-PINOUT.md, which transcribes the
 * pinout shipped with the board and was continuity checked on 2026-09-17.
 *
 * The camera owns GPIO 4-18 except 14, which is the whole reason this board
 * needs its own map: the reference board's I2C pins and all four of its
 * buttons are camera lines here. Nothing below touches a strapping pin
 * (0, 3, 45, 46). The console is USB Serial/JTAG, so UART0's 43/44 carry the
 * encoder (below).
 */

/* Read twice by board.h: first the identity and the kit it can carry, then,
   with the components chosen, the pins. */
#ifndef LEEK_BOARD_PINS

#define BOARD_NAME              "ESP32-S3 CAM"
#define BOARD_MODEL             "LeekWallet-S3CAM"

/*
 * The Zcash screen kit (docs/WIRING-S3CAM.md) is the default: a 1.5" 128x128
 * SH1107 I2C OLED, an EC11 rotary encoder and one Cancel button. The original
 * 128x64 SSD1306 and the four-button module still work on the same pins.
 *
 * The colour kit (docs/WIRING-S3CAM-COLOR.md) is a 240x240 ST7789 module with
 * the encoder and Cancel built in. Its SPI takes K1's and K2's pins, so it
 * goes with the knob only.
 */
#define BOARD_DEFAULT_DISPLAY   LEEK_DISPLAY_SH1107
#define BOARD_DEFAULT_INPUT     LEEK_INPUT_KNOB
#define BOARD_SUPPORTS_DISPLAY(d)  ((d) == LEEK_DISPLAY_SSD1306 || \
                                    (d) == LEEK_DISPLAY_SH1107 ||  \
                                    (d) == LEEK_DISPLAY_ST7789)
#define BOARD_SUPPORTS_INPUT(i)    ((i) == LEEK_INPUT_KNOB ||      \
                                    ((i) == LEEK_INPUT_BUTTONS4 && \
                                     LEEK_DISPLAY != LEEK_DISPLAY_ST7789))

#else /* LEEK_BOARD_PINS */

/*
 * GPIO14 is the one pin in the 4-18 block the camera does not claim. GPIO41 is
 * JTAG MTDI and GPIO42 is MTMS, both free because debugging goes over native
 * USB, and neither is a strapping pin on the S3.
 *
 * GPIO2 is deliberately NOT used as an input, though it is free. It also
 * drives the onboard LED, and an earlier version put K2 there on the
 * assumption that an internal pull-up made this harmless. That assumption was
 * never measured. If the LED sits between GPIO2 and ground, the ~45k internal
 * pull-up is clamped near the LED's forward voltage, roughly 1.8-2.6 V,
 * against a V_IH around 2.5 V - so the idle level is marginal and the button
 * could read permanently pressed. A spare pin costs nothing and an unverified
 * assumption on an input costs a bring-up day, so K2 moved to GPIO41 and
 * GPIO2 is the spare.
 *
 * On the knob kit K1 and K2 are not fitted: their pins keep the internal
 * pull-ups and read released (button.c also ignores a key that reads pressed
 * from power-on). Turning the knob is UP/DOWN; its push switch is on K4's pin,
 * so pressing it is OK.
 */
#if LEEK_DISPLAY == LEEK_DISPLAY_ST7789
/*
 * The colour kit. K1's and K2's pins carry the panel's SPI, so those keys do
 * not exist: GPIO_NUM_NC, which button.c leaves unconfigured. GPIO2 is D/C:
 * an output, where the onboard LED that ruled it out as an input above only
 * flickers with the traffic. CS is grounded on the module and RES is tied to
 * 3V3 through 10k, so neither takes a pin; st7789.c resets the controller
 * with SWRESET instead. BLK goes to 3V3: always on.
 */
#define PIN_BUTTON_UP           GPIO_NUM_NC
#define PIN_BUTTON_DOWN         GPIO_NUM_NC
#define PIN_DISPLAY_SCLK        GPIO_NUM_1    /* module SCL, was K1 */
#define PIN_DISPLAY_MOSI        GPIO_NUM_41   /* module SDA, was K2 - MTDI */
#define PIN_DISPLAY_DC          GPIO_NUM_2
#define PIN_DISPLAY_CS          GPIO_NUM_NC
#define PIN_DISPLAY_RESET       GPIO_NUM_NC
#else
#define PIN_BUTTON_UP           GPIO_NUM_1    /* K1 */
#define PIN_BUTTON_DOWN         GPIO_NUM_41   /* K2 - MTDI */
#endif
#define PIN_BUTTON_CANCEL       GPIO_NUM_14   /* K3; the module's K0 on the colour kit */
#define PIN_BUTTON_ACCEPT       GPIO_NUM_42   /* K4 - MTMS, and the knob's push (PSH) */

/*
 * Encoder on UART0's pins, free because the console is USB Serial/JTAG:
 * CLK on GPIO44 (RX, an input at boot) and DT on GPIO43 (TX). The ROM still
 * prints its boot banner on GPIO43, so DT goes through a 1k resistor: if the
 * contact happens to be closed during those milliseconds, the resistor, not
 * the pin, takes it. Both lines also have the module's 10k pull-ups, which
 * is why its "+" must go to 3V3, never 5V.
 */
#define PIN_ENC_CLK             GPIO_NUM_44
#define PIN_ENC_DT              GPIO_NUM_43

/* The I2C bus: the OLED at 0x3C, and the secure element when one is fitted.
   On the colour kit the bus is the secure element's alone. */
#define PIN_I2C_SDA             GPIO_NUM_47
#define PIN_I2C_SCL             GPIO_NUM_21

/* OV5640 over DVP. Mirrors esp32-camera's BOARD_ESP32S3_WROOM map. PWDN and
   RESET are not wired on this board, hence -1 and the software reset path. */
#define PIN_CAM_PWDN            (-1)
#define PIN_CAM_RESET           (-1)
#define PIN_CAM_XCLK            GPIO_NUM_15
#define PIN_CAM_SIOD            GPIO_NUM_4
#define PIN_CAM_SIOC            GPIO_NUM_5
#define PIN_CAM_D0              GPIO_NUM_11
#define PIN_CAM_D1              GPIO_NUM_9
#define PIN_CAM_D2              GPIO_NUM_8
#define PIN_CAM_D3              GPIO_NUM_10
#define PIN_CAM_D4              GPIO_NUM_12
#define PIN_CAM_D5              GPIO_NUM_18
#define PIN_CAM_D6              GPIO_NUM_17
#define PIN_CAM_D7              GPIO_NUM_16
#define PIN_CAM_VSYNC           GPIO_NUM_6
#define PIN_CAM_HREF            GPIO_NUM_7
#define PIN_CAM_PCLK            GPIO_NUM_13

/* microSD. The board brings out one data line, so 1-bit SDMMC is the only
   mode available. CLK and DATA are also MTCK and MTDO, which means using the
   card rules out EXTERNAL JTAG; native USB JTAG on 19/20 is unaffected. */
#define PIN_SD_CMD              GPIO_NUM_38
#define PIN_SD_CLK              GPIO_NUM_39
#define PIN_SD_D0               GPIO_NUM_40
#define BOARD_SD_BUS_WIDTH      1

/* Onboard WS2812, freed by moving I2C off GPIO48. One pixel, not four. */
#define PIN_PIXELS              GPIO_NUM_48
#define PIXEL_COUNT             1

#define LEEK_HAS_CAMERA         1
#define LEEK_HAS_ZCASH          1
#define LEEK_HAS_SDCARD         1
#define LEEK_HAS_BLE            1   /* present, off at boot, deliberate act to enable */
#define LEEK_HAS_USB            1   /* likewise */
#define LEEK_VAULT_ON_SD        1

#endif /* LEEK_BOARD_PINS */
