# libdspnoise: the Android native Noise provider (M1.10e.1, docs/ANDROID_BLE_DESIGN.md 3.1).
#
# noise-c and libsodium come from the SAME pinned commits and patches as the firmware: the pinning,
# fetch-by-SHA, re-verification, zero-fuzz patching and SHA-256 checks of
# firmware/third_party/noise-c/noise_c_deps.cmake are reused unchanged (decision E3; its toolchain-
# neutral dsp_noise_c_host() builds the "noise_c", "sodium" and "dsp_sodium_hkdf" targets, here
# with the NDK toolchain). Patch 0001 is required on Android: the unpatched noise-c aborts under
# bionic FORTIFY. Nothing is vendored; the first configure needs network access once per cache.
#
# The library contains no deterministic randomness and no responder/fixed-ephemeral path: those
# exist only in the host test library (src/test/native). A post-build check fails the build if a
# forbidden symbol appears or if anything other than the JNI entry points is exported.
cmake_minimum_required(VERSION 3.24)
project(dspnoise C)

set(DSP_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../..")
get_filename_component(DSP_REPO_ROOT "${DSP_REPO_ROOT}" ABSOLUTE)

# One download cache for all ABIs and build types (under the app's build directory), serialized
# with a file lock because Gradle may configure several ABIs in parallel.
if(NOT DEFINED ENV{DSP_DEPS_CACHE} OR "$ENV{DSP_DEPS_CACHE}" STREQUAL "")
    set(ENV{DSP_DEPS_CACHE} "${DSP_REPO_ROOT}/android/app/build/dsp_deps")
endif()
file(MAKE_DIRECTORY "$ENV{DSP_DEPS_CACHE}")
file(LOCK "$ENV{DSP_DEPS_CACHE}/.lock" GUARD PROCESS TIMEOUT 900)
include("${DSP_REPO_ROOT}/firmware/third_party/noise-c/noise_c_deps.cmake")
dsp_noise_c_host()
file(LOCK "$ENV{DSP_DEPS_CACHE}/.lock" RELEASE)

add_library(dspnoise SHARED dspnoise_core.c dspnoise_jni.c)
target_link_libraries(dspnoise PRIVATE noise_c sodium dsp_sodium_hkdf)
target_compile_options(dspnoise PRIVATE -Wall -Wextra -Werror -fstack-protector-strong -fvisibility=hidden)
# FORTIFY stays on: the NDK defines _FORTIFY_SOURCE for optimized builds; dspnoise_core.c refuses to
# compile an optimized Android build without it.
foreach(t noise_c sodium dsp_sodium_hkdf)
    target_compile_options(${t} PRIVATE -fstack-protector-strong -fvisibility=hidden -ffunction-sections -fdata-sections)
endforeach()
target_compile_options(dspnoise PRIVATE -ffunction-sections -fdata-sections)
target_link_options(dspnoise PRIVATE
    -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,relro -Wl,-z,now
    -Wl,-z,max-page-size=16384 -Wl,--version-script=${CMAKE_CURRENT_LIST_DIR}/dspnoise.map)

# Native symbol check: no test-only code, only JNI exports.
find_program(DSP_LLVM_NM llvm-nm HINTS "${ANDROID_TOOLCHAIN_ROOT}/bin" "${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin" REQUIRED)
add_custom_command(TARGET dspnoise POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DNM=${DSP_LLVM_NM} -DLIB=$<TARGET_FILE:dspnoise>
            -P ${CMAKE_CURRENT_LIST_DIR}/check_symbols.cmake
    VERBATIM)
