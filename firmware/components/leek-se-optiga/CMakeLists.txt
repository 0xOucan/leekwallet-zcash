# OPTIGA Trust M bring-up: read-only identity, state and a TRNG smoke test.
# See README.md. optiga-proto.c, optiga-link.c and optiga-cmd.c are pure C and
# also build on the host (sim/test_se_optiga.c); leek-se.c is the ESP-IDF glue.
#
# `driver` for the LEGACY I2C API, deliberately: the panel and the camera's
# SCCB already use it, and ESP-IDF 5.5 aborts at boot if both I2C drivers are
# linked into one image.

#
# LEEK_SE_CONTAINER_WRITES: the development-only Level 1 SE container, the one
# write this component can make (data of 0xF1E0; see README.md). Off unless
# CMake is given -DLEEK_SE_CONTAINER_WRITES=1, e.g. through an env's
# board_build.cmake_extra_args. No env in platformio.ini sets it. PUBLIC so a
# caller's view of leek-se.h agrees with the library it links.

set(srcs "optiga-proto.c" "optiga-link.c" "optiga-cmd.c" "leek-se.c")
if(LEEK_SE_CONTAINER_WRITES)
    list(APPEND srcs "optiga-container.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES driver
    PRIV_REQUIRES mbedtls esp_rom freertos
)

if(LEEK_SE_CONTAINER_WRITES)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC LEEK_SE_CONTAINER_WRITES=1)
endif()
