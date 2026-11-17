# Dispenser V2 firmware (ESP-IDF project, target esp32c3).
# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly.
#
# The application version comes from version.txt in this directory (read by
# ESP-IDF). Without it ESP-IDF would fall back to `git describe`, which reports
# the Android release tags of this monorepo.
cmake_minimum_required(VERSION 3.22)

# ESP-IDF v6.1 nvs_flash with the project's power-loss fixes (third_party/nvs_flash/README.md):
# a verified, patched copy in the build directory overrides the ESP-IDF component; the ESP-IDF
# installation itself is never modified.
include(${CMAKE_CURRENT_LIST_DIR}/third_party/nvs_flash/nvs_flash_override.cmake)
dsp_nvs_flash_override("${CMAKE_BINARY_DIR}/dsp_nvs_flash" DSP_NVS_FLASH_DIR)
list(APPEND EXTRA_COMPONENT_DIRS "${DSP_NVS_FLASH_DIR}")

# M1.10c.2a: the pinned noise-c 0.1.31 with the local patches (third_party/noise-c/README.md),
# verified at configure time; libsodium comes from the ESP-IDF registry at the exact version
# (components/dispenser_security/idf_component.yml, dependencies.lock).
include(${CMAKE_CURRENT_LIST_DIR}/third_party/noise-c/noise_c_deps.cmake)
dsp_noise_c_prepare("${CMAKE_BINARY_DIR}/dsp_noise_c" DSP_NOISE_C_DIR)
list(APPEND EXTRA_COMPONENT_DIRS "${DSP_NOISE_C_DIR}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dispenser)

dsp_nvs_flash_check_selected("${DSP_NVS_FLASH_DIR}")
