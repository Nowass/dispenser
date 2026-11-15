# Host-side unit tests for Dispenser V2 firmware domain code.
#
# Standalone CMake project: needs only CMake >= 3.22 and a C11 compiler (GCC or
# Clang). It does NOT use ESP-IDF, the Xtensa/RISC-V toolchain, hardware, serial
# ports, OpenOCD or radios.
#
#   cmake -S test/host -B build-host
#   cmake --build build-host
#   ctest --test-dir build-host --output-on-failure
cmake_minimum_required(VERSION 3.22)
project(dispenser_host_tests LANGUAGES C)

if(DEFINED ENV{IDF_PATH} AND NOT "$ENV{IDF_PATH}" STREQUAL "")
    message(STATUS "IDF_PATH is set but ignored: host tests never use ESP-IDF")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(DSP_HOST_SANITIZERS "Build host tests with AddressSanitizer and UBSan" ON)

set(FW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

# Same version source as the ESP-IDF build. ESP-IDF reads every line of version.txt,
# so require exactly one SemVer line here; anything else would make the two builds
# disagree. Re-configure automatically when the file changes (as ESP-IDF does).
set(DSP_VERSION_FILE "${FW_ROOT}/version.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DSP_VERSION_FILE}")
file(STRINGS "${DSP_VERSION_FILE}" DSP_FW_VERSION)
list(LENGTH DSP_FW_VERSION DSP_FW_VERSION_LINES)
if(NOT DSP_FW_VERSION_LINES EQUAL 1 OR
   NOT DSP_FW_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?(\\+[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "version.txt must contain exactly one SemVer line, got: '${DSP_FW_VERSION}'")
endif()
message(STATUS "Firmware version (version.txt): ${DSP_FW_VERSION}")

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
if(DSP_HOST_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

# Domain library, built from the component's own CMake file (single source list).
add_subdirectory("${FW_ROOT}/components/dispenser_domain" dispenser_domain)

enable_testing()

function(dsp_add_host_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/support")
    target_link_libraries(${name} PRIVATE dispenser_domain)
    target_compile_definitions(${name} PRIVATE DSP_FW_VERSION="${DSP_FW_VERSION}")
    add_test(NAME ${name} COMMAND ${name})
    # Host unit tests are fast; a hang must fail instead of blocking CI.
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
endfunction()

dsp_add_host_test(test_identity test_identity.c)
