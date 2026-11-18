# Composition root. Dependencies are listed explicitly (no implicit "all components").

# M1.9c: the application runtime's endpoint is checked at configure time (explicit endpoint,
# https only, production only with separate approval). See dsp_endpoint_guard.cmake.
include("${CMAKE_CURRENT_LIST_DIR}/dsp_endpoint_guard.cmake")
dsp_endpoint_guard("${CONFIG_DSP_APP_RUNTIME}" "${CONFIG_DSP_APP_CONNECTIVITY}"
                   "${CONFIG_DSP_APP_NOTIFY_URL}" "${CONFIG_DSP_APP_ALLOW_PRODUCTION}" dsp_endpoint_error)
if(dsp_endpoint_error)
    message(FATAL_ERROR "Dispenser endpoint guard: ${dsp_endpoint_error}")
endif()

# M1.10c.2a: the security layer is build-only until the ESP32-C3 hardware RNG acceptance
# (M1.10c.2b) has passed; never together with the production endpoint.
if(CONFIG_DSP_BLE_SECURITY AND CONFIG_DSP_APP_ALLOW_PRODUCTION)
    message(FATAL_ERROR "CONFIG_DSP_BLE_SECURITY is build-only until the M1.10c.2b hardware RNG "
            "acceptance passes; it cannot be combined with CONFIG_DSP_APP_ALLOW_PRODUCTION")
endif()

# M1.10d.1: the BLE transport and the bring-up window are build-only until M1.10c.2b and M1.10d.2.
if((CONFIG_DSP_BLE_TRANSPORT OR CONFIG_DSP_BLE_BRINGUP_POWER_ON_WINDOW) AND CONFIG_DSP_APP_ALLOW_PRODUCTION)
    message(FATAL_ERROR "CONFIG_DSP_BLE_TRANSPORT / CONFIG_DSP_BLE_BRINGUP_POWER_ON_WINDOW are build-only "
            "until the M1.10c.2b and M1.10d.2 hardware acceptance pass; they cannot be combined with "
            "CONFIG_DSP_APP_ALLOW_PRODUCTION")
endif()

# RNG-gate diagnostic (M1.10c.2b evidence; docs/ANDROID_BLE_DESIGN.md Appendix B): a separate,
# build-only image, never a production image. Kconfig dependencies already exclude the runtime
# (and with it the production endpoint); this is the second, independent check. The fragment
# check also fails a build that asked for sdkconfig.rnggate.defaults but ended up without the
# diagnostic (e.g. a conflicting option silently dropped it) or together with production.
if(CONFIG_DSP_RNG_GATE_DIAGNOSTIC AND CONFIG_DSP_APP_ALLOW_PRODUCTION)
    message(FATAL_ERROR "CONFIG_DSP_RNG_GATE_DIAGNOSTIC is a build-only diagnostic image; it cannot "
            "be combined with CONFIG_DSP_APP_ALLOW_PRODUCTION")
endif()
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    # CONFIG_DSP_APP_ALLOW_PRODUCTION is also looked for in the defaults files themselves: Kconfig
    # silently drops it when its dependencies are unmet, which must not hide such a request.
    idf_build_get_property(_dsp_defaults SDKCONFIG_DEFAULTS)
    idf_build_get_property(_dsp_project_dir PROJECT_DIR)
    set(_dsp_rnggate_requested FALSE)
    set(_dsp_prod_requested FALSE)
    foreach(_dsp_d IN LISTS _dsp_defaults)
        get_filename_component(_dsp_dn "${_dsp_d}" NAME)
        get_filename_component(_dsp_dp "${_dsp_d}" ABSOLUTE BASE_DIR "${_dsp_project_dir}")
        if(_dsp_dn STREQUAL "sdkconfig.rnggate.defaults")
            set(_dsp_rnggate_requested TRUE)
        endif()
        if(EXISTS "${_dsp_dp}")
            file(STRINGS "${_dsp_dp}" _dsp_prod REGEX "^[ \t]*CONFIG_DSP_APP_ALLOW_PRODUCTION[ \t]*=[ \t]*y")
            if(_dsp_prod)
                set(_dsp_prod_requested TRUE)
            endif()
        endif()
    endforeach()
    if(_dsp_rnggate_requested AND (NOT CONFIG_DSP_RNG_GATE_DIAGNOSTIC OR CONFIG_DSP_APP_ALLOW_PRODUCTION
                                   OR _dsp_prod_requested))
        message(FATAL_ERROR "sdkconfig.rnggate.defaults was requested, but the configuration is not a "
                "valid RNG-gate diagnostic image (CONFIG_DSP_RNG_GATE_DIAGNOSTIC="
                "'${CONFIG_DSP_RNG_GATE_DIAGNOSTIC}', CONFIG_DSP_APP_ALLOW_PRODUCTION effective="
                "'${CONFIG_DSP_APP_ALLOW_PRODUCTION}', requested in a defaults file="
                "${_dsp_prod_requested}); the diagnostic requires the BT controller only and "
                "excludes the runtime, BLE security/transport, connectivity, the other diagnostics "
                "and the production endpoint")
    endif()
endif()

# bt and esphome__libsodium are already part of every build (dispenser_platform requires them);
# main uses their headers only in the RNG-gate diagnostic. Listed unconditionally because the
# requirement expansion runs before the configuration is known.
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES dispenser_domain dispenser_platform dispenser_security esp_app_format log
                                freertos esp_hw_support
                       PRIV_REQUIRES bt esphome__libsodium)

if(CONFIG_DSP_RNG_GATE_DIAGNOSTIC)
    # The BT controller library (as in dispenser_platform): esp_bt_controller_* in the diagnostic.
    idf_component_get_property(_dsp_bt_lib bt COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${_dsp_bt_lib})
endif()

if(CONFIG_DSP_RNG_GATE_PSK_VECTORS)
    # R6: the public "psk" / "psk_not" records of test-vectors/ble_session_v1.txt become a C header
    # at configure time (never hand-copied). Strict: an unparsable psk/psk_not line fails the build.
    get_filename_component(_dsp_vec "${CMAKE_CURRENT_LIST_DIR}/../../test-vectors/ble_session_v1.txt"
                           ABSOLUTE)
    if(NOT EXISTS "${_dsp_vec}")
        message(FATAL_ERROR "RNG gate: vector file not found: ${_dsp_vec}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dsp_vec}")
    # REGEX: only the records used here (the file header contains "[", which would break CMake
    # list handling of the other lines)
    file(STRINGS "${_dsp_vec}" _dsp_lines REGEX "^psk(_not)? ")
    set(_dsp_rows "")
    set(_dsp_n_psk 0)
    set(_dsp_n_not 0)
    set(_h "[0-9a-f]")
    foreach(_l IN LISTS _dsp_lines)
        set(_re "^(psk|psk_not) name=([A-Za-z0-9_]+) purpose=([1-4]) setup=x:(${_h}+) token=(-|x:${_h}+) psk=x:(${_h}+)( wrong=x:(${_h}+))?$")
        if(NOT _l MATCHES "${_re}")
            message(FATAL_ERROR "RNG gate: unparsable vector line: ${_l}")
        endif()
        set(_kind "${CMAKE_MATCH_1}")
        set(_name "${CMAKE_MATCH_2}")
        set(_purpose "${CMAKE_MATCH_3}")
        set(_setup "${CMAKE_MATCH_4}")
        set(_token "${CMAKE_MATCH_5}")
        set(_psk "${CMAKE_MATCH_6}")
        set(_wrong "${CMAKE_MATCH_8}")
        string(LENGTH "${_setup}" _ls)
        string(LENGTH "${_psk}" _lp)
        if(NOT _ls EQUAL 32 OR NOT _lp EQUAL 64)
            message(FATAL_ERROR "RNG gate: bad field length in vector ${_name}")
        endif()
        if(_token STREQUAL "-")
            set(_has_token 0)
            set(_token "00000000000000000000000000000000")
        else()
            set(_has_token 1)
            string(SUBSTRING "${_token}" 2 -1 _token)
            string(LENGTH "${_token}" _lt)
            if(NOT _lt EQUAL 32)
                message(FATAL_ERROR "RNG gate: bad token length in vector ${_name}")
            endif()
        endif()
        if(_kind STREQUAL "psk_not")
            string(LENGTH "${_wrong}" _lw)
            if(NOT _lw EQUAL 64)
                message(FATAL_ERROR "RNG gate: psk_not vector ${_name} needs wrong=x:<32 bytes>")
            endif()
            set(_is_not 1)
            math(EXPR _dsp_n_not "${_dsp_n_not} + 1")
        else()
            if(NOT _wrong STREQUAL "")
                message(FATAL_ERROR "RNG gate: psk vector ${_name} must not carry wrong=")
            endif()
            set(_wrong "0000000000000000000000000000000000000000000000000000000000000000")
            set(_is_not 0)
            math(EXPR _dsp_n_psk "${_dsp_n_psk} + 1")
        endif()
        foreach(_f setup token psk wrong)
            string(REGEX REPLACE "(..)" "0x\\1," _${_f}_c "${_${_f}}")
        endforeach()
        string(APPEND _dsp_rows "    {${_is_not}, ${_purpose}, ${_has_token}, {${_setup_c}}, {${_token_c}}, "
               "{${_psk_c}}, {${_wrong_c}}}, /* ${_name} */\n")
    endforeach()
    if(_dsp_n_psk EQUAL 0 OR _dsp_n_not EQUAL 0)
        message(FATAL_ERROR "RNG gate: no psk/psk_not records found in ${_dsp_vec}")
    endif()
    string(CONCAT _dsp_hdr "/* Generated by main/CMakeLists.txt from test-vectors/ble_session_v1.txt. Do not edit. */\n"
        "#define DSP_RNG_GATE_N_PSK ${_dsp_n_psk}\n#define DSP_RNG_GATE_N_PSK_NOT ${_dsp_n_not}\n"
        "static const dsp_rng_gate_vector_t DSP_RNG_GATE_VECTORS[] = {\n${_dsp_rows}};\n")
    set(_dsp_gen "${CMAKE_CURRENT_BINARY_DIR}/dsp_rng_gate_vectors")
    file(CONFIGURE OUTPUT "${_dsp_gen}/dsp_rng_gate_vectors.h" CONTENT "${_dsp_hdr}" @ONLY)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${_dsp_gen}")
endif()
