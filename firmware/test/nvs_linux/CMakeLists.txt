# M1.10b.1: domain storage over the REAL ESP-IDF v6.1 NVS on the linux target (emulated flash
# with power-off injection), with the committed partition table. Not a CTest (needs ESP-IDF):
#   . $HOME/.espressif/tools/activate_idf_v6.1.sh
#   cd firmware/test/nvs_linux && idf.py -B <build-dir> build && <build-dir>/dsp_nvs_linux.elf
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/dispenser_domain"
    "$ENV{IDF_PATH}/tools/mocks/freertos/")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(dsp_nvs_linux)
