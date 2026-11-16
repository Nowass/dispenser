# Build-time guard for the M1.9c application runtime's notification endpoint.
#
# Narrow on purpose: it only answers "may this image's application runtime talk to this
# endpoint?". Included by main/CMakeLists.txt (fails the configure step) and run by the host
# regression test test/host/test_endpoint_guard.cmake. The M1.7 setting DSP_NOTIFY_URL (used by
# the off-by-default HTTPS diagnostic) is not affected.
#
# Rules (approved 2026-11-16):
#  - runtime or connectivity disabled: nothing to check;
#  - connectivity enabled: CONFIG_DSP_APP_NOTIFY_URL must be set explicitly (no fallback to
#    the production default) and must be an https:// URL with a host;
#  - the production host additionally needs CONFIG_DSP_APP_ALLOW_PRODUCTION.

# The deployed production Worker (see cloudflare/dispenser-notify; also the M1.7 default of
# DSP_NOTIFY_URL, which test_endpoint_guard.cmake cross-checks).
set(DSP_PRODUCTION_NOTIFY_HOST "dispenser-notify.nowassek.workers.dev")

# Host of an https:// URL, normalized (lowercase, no userinfo, port or trailing dot).
# Empty if the URL is not https:// or has no host.
function(dsp_url_host url out)
    string(TOLOWER "${url}" u)
    string(STRIP "${u}" u)
    if(NOT u MATCHES "^https://([^/?#]*)")
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    set(authority "${CMAKE_MATCH_1}")
    string(REGEX REPLACE "^.*@" "" authority "${authority}") # drop userinfo
    string(REGEX REPLACE ":[0-9]*$" "" authority "${authority}") # drop port
    string(REGEX REPLACE "\\.+$" "" authority "${authority}") # drop trailing dots
    set(${out} "${authority}" PARENT_SCOPE)
endfunction()

# Sets ${out} to an error message, or to "" when the configuration is acceptable.
function(dsp_endpoint_guard runtime connectivity url allow_production out)
    set(${out} "" PARENT_SCOPE)
    if(NOT runtime OR NOT connectivity)
        return()
    endif()
    string(STRIP "${url}" trimmed)
    if(trimmed STREQUAL "")
        set(${out} "CONFIG_DSP_APP_CONNECTIVITY requires an explicit CONFIG_DSP_APP_NOTIFY_URL \
(a staging endpoint); there is no fallback to the production endpoint" PARENT_SCOPE)
        return()
    endif()
    dsp_url_host("${trimmed}" host)
    if(host STREQUAL "")
        set(${out} "CONFIG_DSP_APP_NOTIFY_URL must be an https:// URL with a host" PARENT_SCOPE)
        return()
    endif()
    if(host STREQUAL DSP_PRODUCTION_NOTIFY_HOST AND NOT allow_production)
        set(${out} "CONFIG_DSP_APP_NOTIFY_URL points to the PRODUCTION endpoint; this needs the \
separate explicit approval CONFIG_DSP_APP_ALLOW_PRODUCTION" PARENT_SCOPE)
        return()
    endif()
endfunction()
