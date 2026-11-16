# Domain storage over the REAL ESP-IDF v6.1 NVS on the linux target (emulated flash with
# power-off injection), with the committed partition table. Not a CTest (needs ESP-IDF); use
# run_checks.sh, which builds the patched component and the unpatched negative control.
#   NVS_UNPATCHED=1  build against ESP-IDF's original nvs_flash (negative control)
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/dispenser_domain"
    "$ENV{IDF_PATH}/tools/mocks/freertos/")
if(NOT "$ENV{NVS_UNPATCHED}" STREQUAL "1")
    include(${CMAKE_CURRENT_LIST_DIR}/../../third_party/nvs_flash/nvs_flash_override.cmake)
    dsp_nvs_flash_override("${CMAKE_BINARY_DIR}/dsp_nvs_flash" DSP_NVS_FLASH_DIR)
    list(APPEND EXTRA_COMPONENT_DIRS "${DSP_NVS_FLASH_DIR}")
else()
    message(STATUS "nvs_flash: UNPATCHED ESP-IDF component (negative control)")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(dsp_nvs_linux)
if(NOT "$ENV{NVS_UNPATCHED}" STREQUAL "1")
    dsp_nvs_flash_check_selected("${DSP_NVS_FLASH_DIR}")
endif()
