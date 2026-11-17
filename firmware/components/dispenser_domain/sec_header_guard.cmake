# Build-time boundary check for the restricted security-layer interface (M1.10b.3,
# docs/OWNERSHIP_DESIGN.md section 7). dsp_app_sec.h may be included only by its implementation
# (dsp_app.c), the future security layer (dsp_blesec.c, M1.10c) and the host tests. This check is
# DEFENSE IN DEPTH: every dsp_app_sec_* function also validates the registered security layer,
# the button session, the handshake evidence and the ownership state at runtime.
#
# dsp_sec_header_guard(<root> <out_var>): scans <root>/components and <root>/main for C/C++
# sources and headers that include "dsp_app_sec.h"; <out_var> = list of violating files ("" ok).
function(dsp_sec_header_guard root out_var)
    file(GLOB_RECURSE candidates
         "${root}/components/*.c" "${root}/components/*.h" "${root}/components/*.cpp"
         "${root}/main/*.c" "${root}/main/*.h" "${root}/main/*.cpp")
    set(bad "")
    foreach(f IN LISTS candidates)
        get_filename_component(name "${f}" NAME)
        if(name STREQUAL "dsp_app.c" OR name STREQUAL "dsp_blesec.c" OR name STREQUAL "dsp_app_sec.h")
            continue()
        endif()
        file(STRINGS "${f}" hits REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]dsp_app_sec\\.h[>\"]")
        if(hits)
            list(APPEND bad "${f}")
        endif()
    endforeach()
    set(${out_var} "${bad}" PARENT_SCOPE)
endfunction()
