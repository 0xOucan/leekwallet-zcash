# LeekWallet main component

FILE(GLOB app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

# One panel driver per board, and never two.
#
# `oled.c` is the SSD1306 over I2C; `oled-pixie.c` is the ST7789 over SPI. Both
# implement the same seven transport entry points, so building both would be a
# duplicate-symbol link error — and the glob above would happily produce one.
# `IDF_TARGET` rather than a CONFIG_* symbol, because component sources are
# collected before sdkconfig is loaded.
if(IDF_TARGET STREQUAL "esp32c3")
  list(REMOVE_ITEM app_sources ${CMAKE_SOURCE_DIR}/src/oled.c)
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
else()
  list(REMOVE_ITEM app_sources ${CMAKE_SOURCE_DIR}/src/oled-pixie.c)
endif()

set(app_requires driver nvs_flash esp_hw_support bootloader_support
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
    # firefly-display is registered empty off the C3, so requiring it always is
    # harmless and keeps this list from needing its own branch.
    PRIV_REQUIRES firefly-display
)

if(LEEK_CAMERA_DRIVER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LEEK_CAMERA_DRIVER=1)
endif()
