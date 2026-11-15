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
    "src/dsp_exec.c"
    "src/dsp_history.c"
    "src/dsp_identity.c"
    "src/dsp_recurrence.c"
    "src/dsp_time.c"
)

if(ESP_PLATFORM)
    # No REQUIRES: the domain depends on no other component. (ESP-IDF still adds
    # its common requirements implicitly; the host build is the real check.)
    idf_component_register(SRCS ${DSP_DOMAIN_SRCS}
                           INCLUDE_DIRS "include")
else()
    add_library(dispenser_domain STATIC ${DSP_DOMAIN_SRCS})
    target_include_directories(dispenser_domain PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
endif()
