# Configure-time guard for the approved flash layout (M1.7, P1/D3).
#
# Why: ESP-IDF v6.1 re-derives values marked "# default:" in sdkconfig from sdkconfig.defaults
# at every reconfigure, but keeps values that were set explicitly (menuconfig, a hand edit, an
# older ESP-IDF that wrote no markers). Such an explicit value wins over sdkconfig.defaults and
# would silently build an image for another layout (the pre-M1.7 layout was 2 MB with a
# single-app table at 0x8000). This guard fails the configure step of every build (and so
# `idf.py flash`, which builds first) whose effective configuration is not the approved layout.
# It cannot protect a flash command that writes an existing build directory without building:
# that is how the 2026-11-19 incident happened (HARDWARE_ACCEPTANCE.md section 1, incident
# record; README "Build").
#
# Included by main/CMakeLists.txt and run by the host regression test
# test/host/test_flash_layout_guard.cmake (which also cross-checks these values against
# sdkconfig.defaults). Change the layout only together with sdkconfig.defaults, partitions.csv
# and a reviewed reason.

set(DSP_LAYOUT_TARGET "esp32c3")
set(DSP_LAYOUT_FLASHSIZE "4MB")
set(DSP_LAYOUT_TABLE "partitions.csv")
set(DSP_LAYOUT_TABLE_OFFSET "0x10000")

# out: "" when the configuration is the approved layout, otherwise the error message.
# Arguments are the raw Kconfig values (booleans "y" or empty).
function(dsp_flash_layout_guard target flashsize custom table offset out)
    set(problems "")
    if(NOT "${target}" STREQUAL "${DSP_LAYOUT_TARGET}")
        list(APPEND problems "CONFIG_IDF_TARGET=${target} (expected ${DSP_LAYOUT_TARGET})")
    endif()
    if(NOT "${flashsize}" STREQUAL "${DSP_LAYOUT_FLASHSIZE}")
        list(APPEND problems "CONFIG_ESPTOOLPY_FLASHSIZE=${flashsize} (expected ${DSP_LAYOUT_FLASHSIZE})")
    endif()
    if(NOT "${custom}" STREQUAL "y")
        list(APPEND problems "CONFIG_PARTITION_TABLE_CUSTOM is not set (expected y)")
    endif()
    if(NOT "${table}" STREQUAL "${DSP_LAYOUT_TABLE}")
        list(APPEND problems "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=${table} (expected ${DSP_LAYOUT_TABLE})")
    endif()
    string(TOLOWER "${offset}" off)
    if(NOT "${off}" STREQUAL "${DSP_LAYOUT_TABLE_OFFSET}")
        list(APPEND problems "CONFIG_PARTITION_TABLE_OFFSET=${offset} (expected ${DSP_LAYOUT_TABLE_OFFSET})")
    endif()
    if(problems)
        list(JOIN problems "; " joined)
        string(CONCAT msg
            "the effective sdkconfig is not the approved flash layout (${DSP_LAYOUT_FLASHSIZE}, "
            "${DSP_LAYOUT_TABLE} at ${DSP_LAYOUT_TABLE_OFFSET}): ${joined}. An explicitly set value "
            "in sdkconfig wins over sdkconfig.defaults. Move that sdkconfig and its build directory "
            "out of the repository and build again, or build in a fresh directory: "
            "idf.py -B <dir> -D SDKCONFIG=<dir>/sdkconfig build")
        set(${out} "${msg}" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()
