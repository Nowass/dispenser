# M1.10d.1 boundary check (docs/BLE_TRANSPORT_DESIGN.md section 2, rule 1): the BLE transport
# object may reference only the three Application Service producers, the shared fragmentation
# codec and the C library's mem* functions. Any other dsp_app_* operation, the restricted
# security interface, the protocol gate, the security layer or noise-c is a build failure.
# Run: cmake -DNM=<nm> -DOBJ=<dsp_ble_transport object> -P test_ble_transport_symbols.cmake
if(NOT NM OR NOT OBJ)
    message(FATAL_ERROR "NM and OBJ must be set")
endif()
execute_process(COMMAND "${NM}" -u "${OBJ}" OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nm failed (${rc})")
endif()
set(allowed dsp_app_post_frame dsp_app_post_conn_event dsp_app_post_rx_fault
            dsp_ble_frag_count dsp_ble_frag_get dsp_ble_reasm_init dsp_ble_reasm_push
            memcpy memset memcmp)
string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_.@]*\n" syms "${out}")
set(bad "")
set(seen "")
foreach(s IN LISTS syms)
    string(STRIP "${s}" s)
    string(REGEX REPLACE "@.*$" "" s "${s}")
    if(s MATCHES "^(__asan|__ubsan|__sanitizer|__tsan|_GLOBAL_OFFSET_TABLE_|__stack_chk)")
        continue() # instrumentation of the host build
    endif()
    list(FIND allowed "${s}" i)
    if(i LESS 0)
        list(APPEND bad "${s}")
    else()
        list(APPEND seen "${s}")
    endif()
endforeach()
if(NOT bad STREQUAL "")
    message(FATAL_ERROR "dsp_ble_transport references forbidden symbols: ${bad}")
endif()
foreach(req dsp_app_post_frame dsp_app_post_conn_event dsp_app_post_rx_fault dsp_ble_reasm_push)
    list(FIND seen "${req}" i)
    if(i LESS 0)
        message(FATAL_ERROR "expected reference missing (check not meaningful): ${req}")
    endif()
endforeach()
message(STATUS "dsp_ble_transport external references: ${seen}")
