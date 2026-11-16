# Composition root. Dependencies are listed explicitly (no implicit "all components").

# M1.9c: the application runtime's endpoint is checked at configure time (explicit endpoint,
# https only, production only with separate approval). See dsp_endpoint_guard.cmake.
include("${CMAKE_CURRENT_LIST_DIR}/dsp_endpoint_guard.cmake")
dsp_endpoint_guard("${CONFIG_DSP_APP_RUNTIME}" "${CONFIG_DSP_APP_CONNECTIVITY}"
                   "${CONFIG_DSP_APP_NOTIFY_URL}" "${CONFIG_DSP_APP_ALLOW_PRODUCTION}" dsp_endpoint_error)
if(dsp_endpoint_error)
    message(FATAL_ERROR "Dispenser endpoint guard: ${dsp_endpoint_error}")
endif()

idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES dispenser_domain dispenser_platform esp_app_format log freertos esp_hw_support)
