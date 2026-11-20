# dispenser_domain: pure business logic, no ESP-IDF APIs.
#
# The same file serves two builds:
#   - ESP-IDF (ESP_PLATFORM defined): registered as an IDF component.
#   - Host (test/host): plain static library compiled with the system compiler,
#     without any ESP-IDF include paths. A stray ESP-IDF include therefore breaks
#     the host build, which enforces the dependency boundary.
#
# Keep the source list here only.
set(DSP_DOMAIN_SRCS
    "src/dsp_app.c"
    "src/dsp_auth.c"
    "src/dsp_bleproto.c"
    "src/dsp_codec.c"
    "src/dsp_config.c"
    "src/dsp_connectivity.c"
    "src/dsp_dosing.c"
    "src/dsp_dosing_state.c"
    "src/dsp_evlog.c"
    "src/dsp_exec.c"
    "src/dsp_history.c"
    "src/dsp_https_transport.c"
    "src/dsp_identity.c"
    "src/dsp_link.c"
    "src/dsp_net.c"
    "src/dsp_notify.c"
    "src/dsp_notify_proto.c"
    "src/dsp_notify_state.c"
    "src/dsp_owner.c"
    "src/dsp_power.c"
    "src/dsp_recurrence.c"
    "src/dsp_reservoir.c"
    "src/dsp_schedule_repo.c"
    "src/dsp_storage.c"
    "src/dsp_text.c"
    "src/dsp_time.c"
    "src/dsp_time_service.c"
    "src/dsp_timekeeping.c"
    "src/dsp_wakeup.c"
)

# M1.10b.3: only dsp_app.c, the future dsp_blesec.c and host tests may include dsp_app_sec.h.
include("${CMAKE_CURRENT_LIST_DIR}/sec_header_guard.cmake")
dsp_sec_header_guard("${CMAKE_CURRENT_LIST_DIR}/../.." _dsp_sec_violations)
if(_dsp_sec_violations)
    message(FATAL_ERROR "dsp_app_sec.h is restricted to the security layer; included by: ${_dsp_sec_violations}")
endif()

if(ESP_PLATFORM)
    # No REQUIRES: the domain depends on no other component. (ESP-IDF still adds
    # its common requirements implicitly; the host build is the real check.)
    idf_component_register(SRCS ${DSP_DOMAIN_SRCS}
                           INCLUDE_DIRS "include")
else()
    add_library(dispenser_domain STATIC ${DSP_DOMAIN_SRCS})
    target_include_directories(dispenser_domain PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
endif()
