# ThreadSanitizer driver for the firmware simulator (TEST-ONLY; see README.md here).
# Builds fwsim_tsan from ../fwsim.c plus the same UNMODIFIED firmware sources libdsp_fwsim links,
# with -fsanitize=thread. Not part of the host CTest suite (which uses ASan/UBSan, incompatible
# with TSan), the firmware image, the APK or the Gradle build. Use run.sh.
cmake_minimum_required(VERSION 3.22)
project(dsp_fwsim_tsan LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(FW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
file(STRINGS "${FW_ROOT}/version.txt" DSP_FW_VERSION)

include("${FW_ROOT}/third_party/noise-c/noise_c_deps.cmake")
dsp_noise_c_host()

# Instrument everything, including the domain/security libraries and noise-c.
add_compile_options(-fsanitize=thread -g -O1 -fno-omit-frame-pointer)
add_link_options(-fsanitize=thread)

add_subdirectory("${FW_ROOT}/components/dispenser_domain" dispenser_domain)
add_subdirectory("${FW_ROOT}/components/dispenser_security" dispenser_security)

if(DSP_JDK_HOME)
    set(JNI_INCLUDE_DIRS "${DSP_JDK_HOME}/include" "${DSP_JDK_HOME}/include/linux" "${DSP_JDK_HOME}/include/darwin")
else()
    find_package(JNI REQUIRED)
endif()

set(PLATFORM "${FW_ROOT}/components/dispenser_platform")
add_executable(fwsim_tsan
    fwsim_tsan.c
    "${FW_ROOT}/test/host/support/fake_kv.c"
    "${FW_ROOT}/test/host/support/test_rng.c"
    "${PLATFORM}/src/dsp_ble_transport.c")
target_include_directories(fwsim_tsan PRIVATE "${FW_ROOT}/test/fwsim" "${FW_ROOT}/test/host"
                           "${FW_ROOT}/test/host/support" "${PLATFORM}/include" ${JNI_INCLUDE_DIRS})
target_compile_definitions(fwsim_tsan PRIVATE DSP_VIA_TRANSPORT=1 DSP_FW_VERSION="${DSP_FW_VERSION}")
target_compile_options(fwsim_tsan PRIVATE -Wall -Wextra -Werror
                       -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable)
target_link_libraries(fwsim_tsan PRIVATE dispenser_security dispenser_domain noise_c sodium pthread)
