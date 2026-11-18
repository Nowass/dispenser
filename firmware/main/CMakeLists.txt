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

idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES dispenser_domain dispenser_platform dispenser_security esp_app_format log
                                freertos esp_hw_support)
