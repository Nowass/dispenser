# Host regression test for main/dsp_endpoint_guard.cmake (M1.9c). Run with
#   cmake -P test/host/test_endpoint_guard.cmake
# (registered in CTest as test_endpoint_guard). No network access, no ESP-IDF.

get_filename_component(FW "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${FW}/main/dsp_endpoint_guard.cmake")

set(checks 0)
set(failures 0)

# expect: "ok" or a substring of the expected error.
function(expect name runtime connectivity url allow expect_what)
    dsp_endpoint_guard("${runtime}" "${connectivity}" "${url}" "${allow}" err)
    math(EXPR c "${checks} + 1")
    set(checks ${c} PARENT_SCOPE)
    if(expect_what STREQUAL "ok")
        set(pass FALSE)
        if(err STREQUAL "")
            set(pass TRUE)
        endif()
    else()
        string(FIND "${err}" "${expect_what}" pos)
        set(pass FALSE)
        if(NOT pos EQUAL -1)
            set(pass TRUE)
        endif()
    endif()
    if(NOT pass)
        math(EXPR f "${failures} + 1")
        set(failures ${f} PARENT_SCOPE)
        message("FAIL ${name}: got '${err}', expected '${expect_what}'")
    endif()
endfunction()

set(PROD "https://dispenser-notify.nowassek.workers.dev/api/v1/events")
set(STAGING "https://dispenser-notify-staging.example.workers.dev/api/v1/events")

# The six required cases.
expect("runtime disabled (default)" n n "" n "ok")
expect("runtime enabled, connectivity disabled" y n "" n "ok")
expect("connectivity without an explicit endpoint" y y "" n "explicit CONFIG_DSP_APP_NOTIFY_URL")
expect("explicit staging endpoint" y y "${STAGING}" n "ok")
expect("production without approval" y y "${PROD}" n "PRODUCTION")
expect("production with explicit approval" y y "${PROD}" y "ok")

# Spellings of the production host are all recognized.
expect("uppercase host" y y "https://DISPENSER-NOTIFY.NOWASSEK.WORKERS.DEV/api/v1/events" n "PRODUCTION")
expect("trailing dot" y y "https://dispenser-notify.nowassek.workers.dev./api/v1/events" n "PRODUCTION")
expect("explicit port" y y "https://dispenser-notify.nowassek.workers.dev:443/api/v1/events" n "PRODUCTION")
expect("userinfo" y y "https://user@dispenser-notify.nowassek.workers.dev/api/v1/events" n "PRODUCTION")
expect("whitespace" y y "  ${PROD}  " n "PRODUCTION")
expect("approval alone does not need production" y y "${STAGING}" y "ok")
# Malformed endpoints.
expect("whitespace only" y y "   " n "explicit CONFIG_DSP_APP_NOTIFY_URL")
expect("plain http" y y "http://staging.example/api/v1/events" n "https://")
expect("no host" y y "https:///api/v1/events" n "https://")
expect("lookalike is not production" y y "https://dispenser-notify.nowassek.workers.dev.example.com/x" n "ok")
# Connectivity flag without the runtime (Kconfig forbids it anyway): nothing to check.
expect("connectivity without runtime" n y "" n "ok")

# The guard's production host is the M1.7 default of DSP_NOTIFY_URL.
file(READ "${FW}/components/dispenser_platform/Kconfig" kconfig)
string(REGEX MATCH "config DSP_NOTIFY_URL[^\n]*\n[^\n]*\n[ ]*default \"([^\"]*)\"" _m "${kconfig}")
dsp_url_host("${CMAKE_MATCH_1}" kconfig_host)
math(EXPR checks "${checks} + 1")
if(NOT kconfig_host STREQUAL DSP_PRODUCTION_NOTIFY_HOST)
    math(EXPR failures "${failures} + 1")
    message("FAIL production host '${DSP_PRODUCTION_NOTIFY_HOST}' != DSP_NOTIFY_URL default host '${kconfig_host}'")
endif()

message("${checks} checks, ${failures} failed")
if(failures GREATER 0)
    message(FATAL_ERROR "endpoint guard regression failed")
endif()
