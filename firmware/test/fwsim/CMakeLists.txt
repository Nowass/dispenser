# Firmware-in-the-loop simulator library for the Android client tests (M1.10e.2b, decision E10).
# TEST-ONLY: builds libdsp_fwsim.so (JNI) from the UNMODIFIED firmware domain, security and BLE
# transport sources plus the host test harness world (test/host). Never part of a firmware image
# or the APK. Built by the Android Gradle task buildFirmwareSim (system compiler, no sanitizers:
# the library is loaded into a JVM), and by the firmware host build as a compile check.
#
#   cmake -S test/fwsim -B build-fwsim -DDSP_JDK_HOME=<jdk> && cmake --build build-fwsim
cmake_minimum_required(VERSION 3.22)
project(dsp_fwsim LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(FW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
file(STRINGS "${FW_ROOT}/version.txt" DSP_FW_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FW_ROOT}/version.txt")

include("${FW_ROOT}/third_party/noise-c/noise_c_deps.cmake")
dsp_noise_c_host()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
add_subdirectory("${FW_ROOT}/components/dispenser_domain" dispenser_domain)
add_subdirectory("${FW_ROOT}/components/dispenser_security" dispenser_security)

if(DSP_JDK_HOME)
    set(JNI_INCLUDE_DIRS "${DSP_JDK_HOME}/include" "${DSP_JDK_HOME}/include/linux" "${DSP_JDK_HOME}/include/darwin")
else()
    find_package(JNI REQUIRED)
endif()

set(PLATFORM "${FW_ROOT}/components/dispenser_platform")
add_library(dsp_fwsim SHARED
    fwsim.c
    "${FW_ROOT}/test/host/support/fake_kv.c"
    "${FW_ROOT}/test/host/support/test_rng.c"
    "${PLATFORM}/src/dsp_ble_transport.c"
    "${PLATFORM}/src/dsp_pump_gpio.c")
target_include_directories(dsp_fwsim PRIVATE "${FW_ROOT}/test/host" "${FW_ROOT}/test/host/support"
                           "${PLATFORM}/include" ${JNI_INCLUDE_DIRS})
target_compile_definitions(dsp_fwsim PRIVATE DSP_VIA_TRANSPORT=1 DSP_FW_VERSION="${DSP_FW_VERSION}")
# the harness sources (test_app_ops.c, test_blesec.c) are test code with helpers fwsim does not use
target_compile_options(dsp_fwsim PRIVATE -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable)
target_link_libraries(dsp_fwsim PRIVATE dispenser_security dispenser_domain noise_c sodium pthread)
# Only the JNI entry points are exported; the library's own noise-c/libsodium copies never mix
# with the phone-side test library loaded into the same JVM.
target_link_options(dsp_fwsim PRIVATE -Wl,--version-script=${CMAKE_CURRENT_LIST_DIR}/fwsim.map
                    -Wl,--exclude-libs,ALL -Wl,-Bsymbolic)
