cmake_minimum_required(VERSION 3.16)
set(SDKCONFIG_DEFAULTS ${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../components/zsign-rust
                         ${CMAKE_CURRENT_LIST_DIR}/../../components/zsign-c)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(zspike)
