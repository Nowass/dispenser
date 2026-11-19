# Host regression test for main/dsp_flash_layout_guard.cmake (M1.7 layout; incident 2026-11-19).
# Run with
#   cmake -P test/host/test_flash_layout_guard.cmake
# (registered in CTest as test_flash_layout_guard). No ESP-IDF needed.

cmake_policy(SET CMP0007 NEW) # list() keeps the empty Flags column of partitions.csv

get_filename_component(FW "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${FW}/main/dsp_flash_layout_guard.cmake")

set(checks 0)
set(failures 0)

function(fail name msg)
    math(EXPR f "${failures} + 1")
    set(failures ${f} PARENT_SCOPE)
    message("FAIL ${name}: ${msg}")
endfunction()

# expect: "ok" or a list of substrings that must all appear in the error.
function(expect name target size custom table offset)
    dsp_flash_layout_guard("${target}" "${size}" "${custom}" "${table}" "${offset}" err)
    math(EXPR c "${checks} + 1")
    set(checks ${c} PARENT_SCOPE)
    set(wanted ${ARGN})
    if(wanted STREQUAL "ok")
        if(NOT err STREQUAL "")
            math(EXPR f "${failures} + 1")
            set(failures ${f} PARENT_SCOPE)
            message("FAIL ${name}: unexpected error '${err}'")
        endif()
        return()
    endif()
    foreach(w IN LISTS wanted)
        string(FIND "${err}" "${w}" pos)
        if(pos EQUAL -1)
            math(EXPR f "${failures} + 1")
            set(failures ${f} PARENT_SCOPE)
            message("FAIL ${name}: '${w}' missing in '${err}'")
            return()
        endif()
    endforeach()
endfunction()

# The approved layout passes (hex case is irrelevant).
expect("approved layout" esp32c3 4MB y partitions.csv 0x10000 ok)
expect("approved layout, upper-case hex" esp32c3 4MB y partitions.csv 0X10000 ok)

# The pre-M1.7 layout (2 MB, single-app table at 0x8000) set explicitly: every deviation named.
expect("pre-M1.7 layout, explicit" esp32c3 2MB "" partitions.csv 0x8000
       "CONFIG_ESPTOOLPY_FLASHSIZE=2MB" "CONFIG_PARTITION_TABLE_CUSTOM is not set"
       "CONFIG_PARTITION_TABLE_OFFSET=0x8000" "idf.py -B <dir> -D SDKCONFIG=<dir>/sdkconfig build")

# Each single deviation is refused.
expect("flash 2 MB" esp32c3 2MB y partitions.csv 0x10000 "CONFIG_ESPTOOLPY_FLASHSIZE=2MB")
expect("flash 8 MB" esp32c3 8MB y partitions.csv 0x10000 "CONFIG_ESPTOOLPY_FLASHSIZE=8MB")
expect("built-in table" esp32c3 4MB "" partitions.csv 0x10000 "CONFIG_PARTITION_TABLE_CUSTOM is not set")
expect("custom flag n" esp32c3 4MB n partitions.csv 0x10000 "CONFIG_PARTITION_TABLE_CUSTOM is not set")
expect("other table file" esp32c3 4MB y partitions_two_ota.csv 0x10000
       "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=partitions_two_ota.csv")
expect("default offset" esp32c3 4MB y partitions.csv 0x8000 "CONFIG_PARTITION_TABLE_OFFSET=0x8000")
expect("other target" esp32s3 4MB y partitions.csv 0x10000 "CONFIG_IDF_TARGET=esp32s3")
expect("empty configuration" "" "" "" "" ""
       "CONFIG_IDF_TARGET=" "CONFIG_ESPTOOLPY_FLASHSIZE=" "CONFIG_PARTITION_TABLE_OFFSET=")

# The guard's expectations match the committed defaults and the partition table.
file(STRINGS "${FW}/sdkconfig.defaults" defaults REGEX "^CONFIG_")
foreach(line
        "CONFIG_IDF_TARGET=\"${DSP_LAYOUT_TARGET}\""
        "CONFIG_ESPTOOLPY_FLASHSIZE_${DSP_LAYOUT_FLASHSIZE}=y"
        "CONFIG_PARTITION_TABLE_CUSTOM=y"
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"${DSP_LAYOUT_TABLE}\""
        "CONFIG_PARTITION_TABLE_OFFSET=${DSP_LAYOUT_TABLE_OFFSET}")
    math(EXPR checks "${checks} + 1")
    list(FIND defaults "${line}" idx)
    if(idx EQUAL -1)
        fail("sdkconfig.defaults" "missing '${line}'")
    endif()
endforeach()
# partitions.csv: the first partition starts right after the 4 KiB table at the guarded offset,
# the first app slot is ota_0 at 0x30000 and the last partition ends at the 4 MB flash end.
file(STRINGS "${FW}/${DSP_LAYOUT_TABLE}" rows REGEX "^[a-z_0-9]+,")
set(first "")
set(last_end 0)
set(ota0 "")
foreach(row IN LISTS rows)
    string(REGEX REPLACE "[ \t]" "" row "${row}")
    string(REPLACE "," ";" f "${row}")
    list(GET f 0 name)
    list(GET f 3 off)
    list(GET f 4 size)
    if(first STREQUAL "")
        set(first "${off}")
    endif()
    if(name STREQUAL "ota_0")
        set(ota0 "${off}")
    endif()
    math(EXPR end "${off} + ${size}")
    if(end GREATER last_end)
        set(last_end ${end})
    endif()
endforeach()
math(EXPR expect_first "${DSP_LAYOUT_TABLE_OFFSET} + 0x1000" OUTPUT_FORMAT HEXADECIMAL)
string(TOLOWER "${first}" first)
math(EXPR checks "${checks} + 3")
if(NOT first STREQUAL expect_first)
    fail("partitions.csv" "first partition at ${first}, expected ${expect_first}")
endif()
if(NOT ota0 STREQUAL "0x30000")
    fail("partitions.csv" "ota_0 at '${ota0}', expected 0x30000")
endif()
if(NOT last_end EQUAL 4194304)
    fail("partitions.csv" "last partition ends at ${last_end}, expected 4194304 (4 MB)")
endif()

if(failures GREATER 0)
    message(FATAL_ERROR "test_flash_layout_guard: ${failures} of ${checks} checks failed")
endif()
message(STATUS "test_flash_layout_guard: ${checks} checks passed")
