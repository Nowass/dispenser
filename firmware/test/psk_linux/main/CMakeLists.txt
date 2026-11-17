set(SEC "${CMAKE_CURRENT_LIST_DIR}/../../../components/dispenser_security")
idf_component_register(SRCS "psk_linux.c" "${SEC}/src/dsp_secderive.c" "${SEC}/src/dsp_hkdf_psa.c"
                       INCLUDE_DIRS "${SEC}/include"
                       REQUIRES mbedtls dispenser_domain)
get_filename_component(DSP_VECTORS "${CMAKE_CURRENT_LIST_DIR}/../../../../test-vectors/ble_session_v1.txt" ABSOLUTE)
target_compile_definitions(${COMPONENT_LIB} PRIVATE DSP_SESSION_VECTORS="${DSP_VECTORS}")
# ESP-IDF's linux build links mbedTLS's allocator hooks (libmbedtls.a) after TF-PSA-Crypto
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,esp_mbedtls_mem_calloc" "-Wl,-u,esp_mbedtls_mem_free")
