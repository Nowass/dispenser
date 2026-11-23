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

if(DSP_HOST_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

# M1.10c.2a: the pinned, verified, patched noise-c and libsodium (decision C3; network access is
# needed once per DSP_DEPS_CACHE). Added before the project's warning flags: third-party code is
# built with its own warnings but under the same sanitizers.
include("${FW_ROOT}/third_party/noise-c/noise_c_deps.cmake")
dsp_noise_c_host()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)

# Domain library, built from the component's own CMake file (single source list).
add_subdirectory("${FW_ROOT}/components/dispenser_domain" dispenser_domain)
# Security layer (Noise sessions, dispatcher, response cache) over the domain.
add_subdirectory("${FW_ROOT}/components/dispenser_security" dispenser_security)

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
# M1.10a: BLE protocol v1 codec against the shared vectors, plus property and fuzz tests.
dsp_add_host_test(test_bleproto test_bleproto.c)
target_compile_definitions(test_bleproto PRIVATE DSP_VECTORS_DIR="${DSP_VECTORS_DIR}")
dsp_add_host_test(test_recurrence test_recurrence.c)
dsp_add_host_test(test_exec test_exec.c support/fake_store.c)
dsp_add_host_test(test_storage test_storage.c support/fake_kv.c)
dsp_add_host_test(test_time test_time.c support/fake_kv.c)
dsp_add_host_test(test_notify test_notify.c support/fake_kv.c)
dsp_add_host_test(test_proto test_proto.c)
dsp_add_host_test(test_https test_https.c support/fake_kv.c)
dsp_add_host_test(test_dosing test_dosing.c support/fake_kv.c)
# Phase C: persistent event history (separate from the anti-replay records).
dsp_add_host_test(test_evlog test_evlog.c support/fake_kv.c)
dsp_add_host_test(test_app test_app.c support/fake_kv.c
                  "${CMAKE_CURRENT_LIST_DIR}/../../components/dispenser_platform/src/dsp_pump_gpio.c")
target_include_directories(test_app PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../components/dispenser_platform/include")
# M1.10b.1: configuration, schedules and phone time through the Application Service.
dsp_add_host_test(test_app_ops test_app_ops.c support/fake_kv.c)
# M1.10b.2: manual dose with durable anti-replay (reuses the test_app_ops harness).
dsp_add_host_test(test_manual test_manual.c support/fake_kv.c)
target_compile_options(test_manual PRIVATE -Wno-unused-function)
# Battery telemetry through the Application Service (shared harness of test_app_ops.c).
dsp_add_host_test(test_battery_app test_battery_app.c support/fake_kv.c)
dsp_add_host_test(test_ota_app test_ota_app.c support/fake_kv.c)
target_compile_options(test_ota_app PRIVATE -Wno-unused-function)
target_compile_options(test_battery_app PRIVATE -Wno-unused-function)
# Incident 2026-11-20 (F1): READY dosing floor (same harness).
dsp_add_host_test(test_ready_floor test_ready_floor.c support/fake_kv.c)
target_compile_options(test_ready_floor PRIVATE -Wno-unused-function)
dsp_add_host_test(test_button_wake test_button_wake.c support/fake_kv.c)
target_compile_options(test_button_wake PRIVATE -Wno-unused-function)
# Phase C: the event history through the Application Service (same harness).
dsp_add_host_test(test_history_app test_history_app.c support/fake_kv.c)
target_compile_options(test_history_app PRIVATE -Wno-unused-function)
# M1.10b.3: credential record, RTC overlays, confirmation (pure) and ownership through the
# Application Service (reuses the test_app_ops harness).
dsp_add_host_test(test_auth test_auth.c)
# M1.10c.1: pre-authentication protocol gate and the authorization boundary through wakes.
dsp_add_host_test(test_link test_link.c)
dsp_add_host_test(test_session test_session.c support/fake_kv.c)
target_compile_options(test_session PRIVATE -Wno-unused-function)
dsp_add_host_test(test_owner test_owner.c support/fake_kv.c)
target_compile_options(test_owner PRIVATE -Wno-unused-function)
# M1.10c.2a: session-security derivations (independent vectors) and the Noise security session
# end to end through the Application Service (a noise-c "phone" against dsp_blesec).
dsp_add_host_test(test_secderive test_secderive.c)
target_link_libraries(test_secderive PRIVATE dispenser_security sodium)
target_compile_definitions(test_secderive PRIVATE DSP_VECTORS_DIR="${DSP_VECTORS_DIR}")
dsp_add_host_test(test_blesec test_blesec.c support/fake_kv.c support/test_rng.c)
target_link_libraries(test_blesec PRIVATE dispenser_security noise_c sodium)
target_compile_options(test_blesec PRIVATE -Wno-unused-function)
set_tests_properties(test_blesec PROPERTIES TIMEOUT 300)
# M1.9c: the pure platform adapter logic (compiled unchanged into the firmware component;
# no ESP-IDF headers) and a two-thread check of the command-queue boundary.
set(DSP_PLATFORM_DIR "${CMAKE_CURRENT_LIST_DIR}/../../components/dispenser_platform")
dsp_add_host_test(test_platform test_platform.c support/fake_kv.c
                  "${DSP_PLATFORM_DIR}/src/dsp_platform_logic.c")
target_include_directories(test_platform PRIVATE "${DSP_PLATFORM_DIR}/include")
# Bench GPIO pump control signal (pure driver logic, compiled unchanged into the firmware).
dsp_add_host_test(test_pump_gpio test_pump_gpio.c "${DSP_PLATFORM_DIR}/src/dsp_pump_gpio.c")
target_include_directories(test_pump_gpio PRIVATE "${DSP_PLATFORM_DIR}/include")
find_package(Threads REQUIRED)
# M1.9c: build-time endpoint guard of the application runtime (pure CMake script).
add_test(NAME test_sec_header_guard
         COMMAND ${CMAKE_COMMAND} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/sec_guard_tree
                 -P "${CMAKE_CURRENT_LIST_DIR}/test_sec_header_guard.cmake")
add_test(NAME test_endpoint_guard
         COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_LIST_DIR}/test_endpoint_guard.cmake")
# M1.7 flash layout guard (stale sdkconfig incident 2026-11-19; pure CMake script).
add_test(NAME test_flash_layout_guard
         COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_LIST_DIR}/test_flash_layout_guard.cmake")
add_test(NAME test_button_config
         COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_LIST_DIR}/test_button_config.cmake")
target_link_libraries(test_platform PRIVATE Threads::Threads)
# M1.10d.1: the pure BLE byte transport (compiled unchanged into the firmware component) over a
# fault-injecting fake radio, including a two-thread test (simulated NimBLE host task).
dsp_add_host_test(test_ble_transport test_ble_transport.c
                  "${DSP_PLATFORM_DIR}/src/dsp_ble_transport.c")
target_include_directories(test_ble_transport PRIVATE "${DSP_PLATFORM_DIR}/include")
target_link_libraries(test_ble_transport PRIVATE Threads::Threads)
set_tests_properties(test_ble_transport PROPERTIES TIMEOUT 120)
add_library(dsp_ble_transport_obj OBJECT "${DSP_PLATFORM_DIR}/src/dsp_ble_transport.c")
target_include_directories(dsp_ble_transport_obj PRIVATE "${DSP_PLATFORM_DIR}/include")
target_link_libraries(dsp_ble_transport_obj PRIVATE dispenser_domain)
add_test(NAME test_ble_transport_symbols
         COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DOBJ=$<TARGET_OBJECTS:dsp_ble_transport_obj>
                 -P "${CMAKE_CURRENT_LIST_DIR}/test_ble_transport_symbols.cmake")
# M1.10e (E7): the shared handshake transcripts (test-vectors/ble_handshake_v1.txt, generated by the
# independent Rust snow tool) replayed byte for byte through the real dsp_app + dsp_blesec.
dsp_add_host_test(test_handshake_vectors test_handshake_vectors.c support/fake_kv.c support/test_rng.c)
target_link_libraries(test_handshake_vectors PRIVATE dispenser_security noise_c sodium)
target_compile_options(test_handshake_vectors PRIVATE -Wno-unused-function)
target_compile_definitions(test_handshake_vectors PRIVATE DSP_VECTORS_DIR="${DSP_VECTORS_DIR}")
set_tests_properties(test_handshake_vectors PROPERTIES TIMEOUT 300)

# M1.10d.1 end to end: the complete noise-c phone suite (test_blesec.c) through the transport, the
# phone fragmenting into ATT writes and reassembling notifications, at three negotiated MTUs.
dsp_add_host_test(test_ble_e2e test_blesec.c support/fake_kv.c support/test_rng.c
                  "${DSP_PLATFORM_DIR}/src/dsp_ble_transport.c")
target_compile_definitions(test_ble_e2e PRIVATE DSP_VIA_TRANSPORT=1)
target_include_directories(test_ble_e2e PRIVATE "${DSP_PLATFORM_DIR}/include")
target_link_libraries(test_ble_e2e PRIVATE dispenser_security noise_c sodium)
target_compile_options(test_ble_e2e PRIVATE -Wno-unused-function)
set_tests_properties(test_ble_e2e PROPERTIES TIMEOUT 300 ENVIRONMENT "DSP_E2E_MTU=23")
foreach(mtu 64 247)
    add_test(NAME test_ble_e2e_mtu${mtu} COMMAND test_ble_e2e)
    set_tests_properties(test_ble_e2e_mtu${mtu} PROPERTIES TIMEOUT 300 ENVIRONMENT "DSP_E2E_MTU=${mtu}")
endforeach()

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
