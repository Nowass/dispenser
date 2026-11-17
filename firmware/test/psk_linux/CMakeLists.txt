# M1.10c.2a: the DEVICE HKDF provider (mbedTLS / TF-PSA-Crypto PSA, ESP-IDF v6.1 sources and
# esp_config.h) with the production PSK code (dispenser_security dsp_secderive.c, dsp_hkdf_psa.c)
# against the independent vectors (test-vectors/ble_session_v1.txt), on ESP-IDF's linux target.
# Not a CTest (needs ESP-IDF); run run_check.sh. This is NOT the target RNG / on-device PSK check
# (design 6.1 R6, M1.10c.2b): it runs the same library build for the host CPU.
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/dispenser_domain"
    "$ENV{IDF_PATH}/tools/mocks/freertos/")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(dsp_psk_linux)
