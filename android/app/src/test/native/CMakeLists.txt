# Native TEST builds of the Android Noise core (M1.10e.1). Never part of the APK.
#
# Host toolchain (Gradle task buildHostNativeTest; developer machines and CI):
#   - libdspnoise_hosttest: the app's JNI library sources + dspnoise_test_hooks.c, compiled with
#     DSPN_TEST_HOOKS (responder role, fixed ephemerals) for the JVM unit tests;
#   - dspnoise_vectors: the native vector runner.
# Android NDK toolchain, ABI x86_64 (Gradle task nativeAndroidTest; CI):
#   - dspnoise_vectors as a STATIC executable (bionic, FORTIFY on), run directly on the Linux host.
# noise-c and libsodium: the same pinned, verified, patched sources as the app and the firmware.
cmake_minimum_required(VERSION 3.24)
project(dspnoise_tests C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # FORTIFY evidence for the Android runner build

set(APP_CPP "${CMAKE_CURRENT_LIST_DIR}/../../main/cpp")
set(DSP_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../..")
get_filename_component(DSP_REPO_ROOT "${DSP_REPO_ROOT}" ABSOLUTE)
if(NOT DEFINED ENV{DSP_DEPS_CACHE} OR "$ENV{DSP_DEPS_CACHE}" STREQUAL "")
    set(ENV{DSP_DEPS_CACHE} "${DSP_REPO_ROOT}/android/app/build/dsp_deps")
endif()
file(MAKE_DIRECTORY "$ENV{DSP_DEPS_CACHE}")
file(LOCK "$ENV{DSP_DEPS_CACHE}/.lock" GUARD PROCESS TIMEOUT 900)
include("${DSP_REPO_ROOT}/firmware/third_party/noise-c/noise_c_deps.cmake")
dsp_noise_c_host()
file(LOCK "$ENV{DSP_DEPS_CACHE}/.lock" RELEASE)

# noise-c's JSON tokenizer (from the same pinned tree) for the official vector files
_dsp_deps_cache(_cache)
set(NC_SRC "${_cache}/noise-c-${DSP_NOISE_C_COMMIT}")
set(NC_VECTORS "${NC_SRC}/tests/vector" CACHE PATH "noise-c official vector directory" FORCE)

set(WARN -Wall -Wextra -Werror)
add_executable(dspnoise_vectors dspnoise_vectors_test.c "${APP_CPP}/dspnoise_core.c" "${NC_VECTORS}/json-reader.c")
target_include_directories(dspnoise_vectors PRIVATE "${APP_CPP}" "${NC_VECTORS}")
target_compile_definitions(dspnoise_vectors PRIVATE DSPN_TEST_HOOKS=1)
target_link_libraries(dspnoise_vectors PRIVATE noise_c sodium dsp_sodium_hkdf)
target_compile_options(dspnoise_vectors PRIVATE ${WARN} -Wno-format-truncation)
set_source_files_properties("${NC_VECTORS}/json-reader.c" PROPERTIES COMPILE_OPTIONS "-Wno-error;-w")

if(ANDROID)
    target_link_options(dspnoise_vectors PRIVATE -static)
else()
    # JNI headers of the JDK running the tests (Gradle passes its toolchain's home as DSP_JDK_HOME)
    if(DSP_JDK_HOME)
        set(JNI_INCLUDE_DIRS "${DSP_JDK_HOME}/include" "${DSP_JDK_HOME}/include/linux" "${DSP_JDK_HOME}/include/darwin")
    else()
        find_package(JNI REQUIRED)
    endif()
    add_library(dspnoise_hosttest SHARED "${APP_CPP}/dspnoise_core.c" "${APP_CPP}/dspnoise_jni.c" dspnoise_test_hooks.c)
    target_include_directories(dspnoise_hosttest PRIVATE "${APP_CPP}" ${JNI_INCLUDE_DIRS})
    target_compile_definitions(dspnoise_hosttest PRIVATE DSPN_TEST_HOOKS=1)
    target_link_libraries(dspnoise_hosttest PRIVATE noise_c sodium dsp_sodium_hkdf)
    target_compile_options(dspnoise_hosttest PRIVATE ${WARN})
    set_target_properties(noise_c sodium dsp_sodium_hkdf PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
