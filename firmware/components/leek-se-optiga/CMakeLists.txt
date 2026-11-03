# OPTIGA Trust M bring-up: read-only identity, state and a TRNG smoke test.
# See README.md. optiga-proto.c, optiga-link.c and optiga-cmd.c are pure C and
# also build on the host (sim/test_se_optiga.c); leek-se.c is the ESP-IDF glue.
#
# `driver` for the LEGACY I2C API, deliberately: the panel and the camera's
# SCCB already use it, and ESP-IDF 5.5 aborts at boot if both I2C drivers are
# linked into one image.

idf_component_register(
    SRCS "optiga-proto.c" "optiga-link.c" "optiga-cmd.c" "leek-se.c"
    INCLUDE_DIRS "include"
    REQUIRES driver
    PRIV_REQUIRES mbedtls esp_rom freertos
)
