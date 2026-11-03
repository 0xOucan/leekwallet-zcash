# LeekWallet main component

FILE(GLOB app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

# One panel driver per build, and never two.
#
# `oled.c` (the I2C OLEDs) and `oled-st7789.c` (the SPI TFT) implement the
# same transport entry points, so linking both would be a duplicate-symbol
# error. Each one compiles to nothing unless board.h selected its panel
# (BOARD_HAS_I2C_PANEL / BOARD_HAS_SPI_PANEL), so the choice is made once, in
# the header every source already reads, and not again here.
#
# What the C3 cannot build is still removed by target. `IDF_TARGET` rather than
# a CONFIG_* symbol, because component sources are collected before sdkconfig
# is loaded.
if(IDF_TARGET STREQUAL "esp32c3")
  # The Zcash signer is built for Xtensa only (components/zsign-rust).
  list(REMOVE_ITEM app_sources ${CMAKE_SOURCE_DIR}/src/zcash-account.c)
  list(REMOVE_ITEM app_sources ${CMAKE_SOURCE_DIR}/src/zcash-worker.c)
  list(REMOVE_ITEM app_sources ${CMAKE_SOURCE_DIR}/src/zcash-sign.c)
  list(REMOVE_ITEM app_sources ${CMAKE_SOURCE_DIR}/src/zcash-review.c)
  list(REMOVE_ITEM app_sources ${CMAKE_SOURCE_DIR}/src/zcash-receive.c)
  list(REMOVE_ITEM app_sources ${CMAKE_SOURCE_DIR}/src/zcash-fp.c)
  list(REMOVE_ITEM app_sources ${CMAKE_SOURCE_DIR}/src/zcash-meta.c)
  list(REMOVE_ITEM app_sources ${CMAKE_SOURCE_DIR}/src/sd-seed.c)
  list(REMOVE_ITEM app_sources ${CMAKE_SOURCE_DIR}/src/sdcard.c)
  list(REMOVE_ITEM app_sources ${CMAKE_SOURCE_DIR}/src/zkv.c)
endif()

# esp_driver_spi for st7789.c; ESP-IDF 5.x split the SPI master out of
# `driver`. Unused code from it is not linked into an OLED build.
set(app_requires driver esp_driver_spi nvs_flash esp_hw_support bootloader_support
                 esp_driver_usb_serial_jtag trezor-crypto leek-wallet)
if(NOT IDF_TARGET STREQUAL "esp32c3")
    # Both signer backends: BOTH mode requires them to agree (zcash-sign.c).
    list(APPEND app_requires zsign-rust zsign-c)
    # The microSD card (sdcard.c): SDMMC host and FATFS. Every S3 board has
    # the slot or its pins; the C3 has neither.
    list(APPEND app_requires fatfs sdmmc esp_driver_sdmmc)
endif()

# The camera stack, only where the top-level CMakeLists did not exclude it.
# Naming these unconditionally would fail the build on every other environment,
# since a REQUIRES entry that resolves to nothing is an error, not a no-op.
if(LEEK_CAMERA_DRIVER)
    list(APPEND app_requires esp32-camera quirc)
endif()

idf_component_register(
    SRCS ${app_sources}
    REQUIRES ${app_requires}
)

if(LEEK_CAMERA_DRIVER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LEEK_CAMERA_DRIVER=1)
endif()
