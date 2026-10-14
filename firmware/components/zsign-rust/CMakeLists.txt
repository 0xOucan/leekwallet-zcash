# LeekWallet Zcash: Rust signer backend as an ESP-IDF component.
# cargo builds the no_std staticlib for xtensa-esp32s3-none-elf; IDF links it.
idf_component_register(SRCS "platform/zsign_platform_idf.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_hw_support heap log)

set(ZS_TARGET xtensa-esp32s3-none-elf)
set(ZS_CARGO_DIR ${CMAKE_BINARY_DIR}/zsign-rust-target)
set(ZS_LIB ${ZS_CARGO_DIR}/${ZS_TARGET}/release/libzsign_rust.a)

add_custom_target(zsign_rust_cargo
    COMMAND cargo +esp build --release --target ${ZS_TARGET}
            -Zbuild-std=core,alloc --target-dir ${ZS_CARGO_DIR}
    WORKING_DIRECTORY ${COMPONENT_DIR}
    BYPRODUCTS ${ZS_LIB}
    USES_TERMINAL
    COMMENT "cargo: zsign-rust for ${ZS_TARGET}")

add_prebuilt_library(zsign_rust_lib ${ZS_LIB})
add_dependencies(zsign_rust_lib zsign_rust_cargo)
target_link_libraries(${COMPONENT_LIB} INTERFACE zsign_rust_lib)
