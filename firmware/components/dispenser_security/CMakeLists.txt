# dispenser_security (M1.10c.2a, decision C8): the BLE security session (Noise XXpsk3 / IK over
# the pinned, patched noise-c and libsodium), the authorization-bound dispatcher and the response
# cache. Depends on dispenser_domain; the domain never depends on it.
#
# Two builds from this file:
#   - ESP-IDF: HKDF by mbedTLS / TF-PSA-Crypto (PSA), libsodium from the component registry at the
#     exact pinned version (idf_component.yml, dependencies.lock), noise-c from the verified,
#     patched tree (third_party/noise-c/noise_c_deps.cmake, EXTRA_COMPONENT_DIRS).
#   - Host (test/host): HKDF by libsodium's crypto_kdf_hkdf_sha256 (the Android provider), the
#     same noise-c and libsodium commits (dsp_noise_c_host()).
# A deterministic RNG exists only in the host tests (test/host/support); the ESP-IDF build refuses
# any source that defines DSP_TEST_DETERMINISTIC_RNG (see dsp_blesec.c).
set(DSP_SECURITY_SRCS
    "src/dsp_secderive.c"
    "src/dsp_idhash.c"
    "src/dsp_rescache.c"
    "src/dsp_dispatch.c"
    "src/dsp_blesec.c"
    "src/dsp_noise_rand.c"
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${DSP_SECURITY_SRCS} "src/dsp_hkdf_psa.c"
                           INCLUDE_DIRS "include"
                           REQUIRES dispenser_domain
                           PRIV_REQUIRES esphome__noise-c esphome__libsodium mbedtls)
    # noise-c calls the hook defined here (dsp_noise_rand.c): pull it in regardless of link order,
    # only in images that link the security layer (others stay byte-identical)
    if(CONFIG_DSP_BLE_SECURITY)
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,noise_rand_bytes")
    endif()
else()
    add_library(dispenser_security STATIC ${DSP_SECURITY_SRCS} "src/dsp_hkdf_sodium.c")
    target_include_directories(dispenser_security PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(dispenser_security PUBLIC dispenser_domain PRIVATE noise_c sodium dsp_sodium_hkdf)
    target_link_options(dispenser_security INTERFACE "-Wl,-u,noise_rand_bytes")
endif()
