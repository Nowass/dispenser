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

# Shared recurrence vectors (repository root test-vectors/, also used by the Android
# Kotlin tests). Read at run time; the path is fixed at configure time.
set(DSP_VECTORS_DIR "${FW_ROOT}/../test-vectors")
get_filename_component(DSP_VECTORS_DIR "${DSP_VECTORS_DIR}" ABSOLUTE)
dsp_add_host_test(test_recurrence_vectors test_recurrence_vectors.c)
target_compile_definitions(test_recurrence_vectors PRIVATE DSP_VECTORS_DIR="${DSP_VECTORS_DIR}")

# Shared notification protocol v1 vectors (M1.8), also consumed by the backend Vitest suite.
dsp_add_host_test(test_proto_vectors test_proto_vectors.c)
target_compile_definitions(test_proto_vectors PRIVATE DSP_VECTORS_DIR="${DSP_VECTORS_DIR}")
dsp_add_host_test(test_recurrence test_recurrence.c)
dsp_add_host_test(test_exec test_exec.c support/fake_store.c)
dsp_add_host_test(test_storage test_storage.c support/fake_kv.c)
dsp_add_host_test(test_time test_time.c support/fake_kv.c)
dsp_add_host_test(test_notify test_notify.c support/fake_kv.c)
dsp_add_host_test(test_proto test_proto.c)
dsp_add_host_test(test_https test_https.c support/fake_kv.c)
dsp_add_host_test(test_dosing test_dosing.c support/fake_kv.c)
dsp_add_host_test(test_app test_app.c support/fake_kv.c)
# M1.9c: the pure platform adapter logic (compiled unchanged into the firmware component;
# no ESP-IDF headers) and a two-thread check of the command-queue boundary.
set(DSP_PLATFORM_DIR "${CMAKE_CURRENT_LIST_DIR}/../../components/dispenser_platform")
dsp_add_host_test(test_platform test_platform.c support/fake_kv.c
                  "${DSP_PLATFORM_DIR}/src/dsp_platform_logic.c")
target_include_directories(test_platform PRIVATE "${DSP_PLATFORM_DIR}/include")
find_package(Threads REQUIRED)
# M1.9c: build-time endpoint guard of the application runtime (pure CMake script).
add_test(NAME test_endpoint_guard
         COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_LIST_DIR}/test_endpoint_guard.cmake")
target_link_libraries(test_platform PRIVATE Threads::Threads)

# The engine never consults the C library timezone: identical results under any host TZ.
foreach(tz "America/New_York" "Asia/Kathmandu")
    string(REPLACE "/" "_" tz_name "${tz}")
    add_test(NAME test_recurrence_vectors_TZ_${tz_name} COMMAND test_recurrence_vectors)
    set_tests_properties(test_recurrence_vectors_TZ_${tz_name} PROPERTIES
                         ENVIRONMENT "TZ=${tz}" TIMEOUT 30)
endforeach()

# M1.8 integration harness (not a CTest: needs Node.js, OpenSSL and the local Worker; run
# by firmware/test/integration/notify_e2e.mjs). Built only when OpenSSL is available.
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    add_executable(notify_harness
        ${CMAKE_CURRENT_LIST_DIR}/../integration/notify_harness.c
        ${CMAKE_CURRENT_LIST_DIR}/../integration/posix_https_port.c
        ${CMAKE_CURRENT_LIST_DIR}/support/fake_kv.c)
    target_include_directories(notify_harness PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/support" "${CMAKE_CURRENT_LIST_DIR}/../integration")
    target_link_libraries(notify_harness PRIVATE dispenser_domain OpenSSL::SSL OpenSSL::Crypto)
else()
    message(STATUS "OpenSSL not found: the M1.8 integration harness is not built")
endif()
