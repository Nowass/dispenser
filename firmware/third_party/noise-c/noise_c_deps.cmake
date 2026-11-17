# Pinned, verified, patched noise-c and libsodium for the firmware (ESP-IDF) and the host tests
# (M1.10c.2a, decision C3; see README.md in this directory).
#
# Nothing is vendored. At configure time the pinned upstream commits are fetched by their full
# SHA (git verifies content by hash), the checked-out commits are verified again, the Dispenser
# local patches are applied with zero fuzz, and the patched files are verified against pinned
# SHA-256 values. Any mismatch stops the configuration with an explicit error. The ESP-IDF
# installation is never modified.
#
# Downloads land in DSP_DEPS_CACHE (environment variable; default <build>/_dsp_deps), so a fresh
# build directory needs network access once.
#
# ESP-IDF project (before project()):
#   include(.../third_party/noise-c/noise_c_deps.cmake)
#   dsp_noise_c_prepare("${CMAKE_BINARY_DIR}/dsp_noise_c" DSP_NOISE_C_DIR)
#   list(APPEND EXTRA_COMPONENT_DIRS "${DSP_NOISE_C_DIR}")   # component esphome__noise-c
#   (libsodium comes from the ESP-IDF component registry at the exact version, dependencies.lock)
#
# Host (CTest):
#   dsp_noise_c_host()   # targets noise_c, sodium, dsp_sodium_hkdf (host cross-check only)

set(DSP_NOISE_C_URL "https://github.com/esphome/noise-c.git")
set(DSP_NOISE_C_COMMIT "62c2fdca7a874ea898761147bb062ca1cde9f36d")          # v0.1.31
set(DSP_SODIUM_FORK_URL "https://github.com/esphome-libs/libsodium")
set(DSP_SODIUM_FORK_COMMIT "95c5e60d991b3f735619da8a307951c0483ef567")      # 1.10021.12
set(DSP_SODIUM_UPSTREAM_COMMIT "d24faf56214469b354b01c8ba36257e04737101e")  # 1.0.21-RELEASE
set(DSP_NOISE_C_PATCHES
    "0001-patterns-bound-base-pattern-search-by-table-size.patch"
    "0002-esp-idf-keep-default-feature-switches.patch")
# SHA-256 of the two files the patches touch, after patching.
set(DSP_NOISE_C_PATCHED_PATTERNS_SHA256 "3cd9b7dbc7ae4a25b5cddff3a7ab36d32ccf16b7d92a2c55c604bc16b0d88c04")
set(DSP_NOISE_C_PATCHED_CMAKELISTS_SHA256 "9a248c18f365d4b852ad8381335b6f304173abd646160e5905feaf7d1c1d9456")
set(DSP_NOISE_C_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(_dsp_git_checkout url commit dir)
    find_package(Git REQUIRED)
    set(ok FALSE)
    if(EXISTS "${dir}/.git")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" rev-parse HEAD
                        OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc
                        ERROR_QUIET)
        if(rc EQUAL 0 AND head STREQUAL commit)
            set(ok TRUE)
        endif()
    endif()
    if(NOT ok)
        file(REMOVE_RECURSE "${dir}")
        file(MAKE_DIRECTORY "${dir}")
        foreach(cmd "init;-q" "remote;add;origin;${url}" "fetch;-q;--depth;1;origin;${commit}"
                    "checkout;-q;--detach;FETCH_HEAD")
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" ${cmd} RESULT_VARIABLE rc
                            OUTPUT_VARIABLE out ERROR_VARIABLE out)
            if(NOT rc EQUAL 0)
                message(FATAL_ERROR "dsp deps: git ${cmd} failed for ${url} @ ${commit} (network "
                        "access is needed once per DSP_DEPS_CACHE):\n${out}")
            endif()
        endforeach()
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" rev-parse HEAD
                    OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT head STREQUAL commit)
        message(FATAL_ERROR "dsp deps: ${url} checked out ${head}, pinned ${commit}")
    endif()
endfunction()

function(_dsp_deps_cache out)
    if(DEFINED ENV{DSP_DEPS_CACHE} AND NOT "$ENV{DSP_DEPS_CACHE}" STREQUAL "")
        set(${out} "$ENV{DSP_DEPS_CACHE}" PARENT_SCOPE)
    else()
        set(${out} "${CMAKE_BINARY_DIR}/_dsp_deps" PARENT_SCOPE)
    endif()
endfunction()

# Patched noise-c tree at <dst_root>/esphome__noise-c (directory name = ESP-IDF component name).
function(dsp_noise_c_prepare dst_root out_dir)
    _dsp_deps_cache(cache)
    set(src "${cache}/noise-c-${DSP_NOISE_C_COMMIT}")
    _dsp_git_checkout("${DSP_NOISE_C_URL}" "${DSP_NOISE_C_COMMIT}" "${src}")
    set(dst "${dst_root}/esphome__noise-c")
    set(patch_hashes "")
    foreach(p IN LISTS DSP_NOISE_C_PATCHES)
        file(SHA256 "${DSP_NOISE_C_MODULE_DIR}/patches/${p}" h)
        string(APPEND patch_hashes "${h};")
    endforeach()
    string(SHA256 stamp "${DSP_NOISE_C_COMMIT};${patch_hashes}")
    set(stamp_file "${dst_root}/stamp.txt")
    set(current "")
    if(EXISTS "${stamp_file}")
        file(READ "${stamp_file}" current)
    endif()
    if(NOT current STREQUAL stamp OR NOT EXISTS "${dst}/src/protocol/patterns.c")
        find_program(DSP_PATCH_EXECUTABLE patch)
        if(NOT DSP_PATCH_EXECUTABLE)
            message(FATAL_ERROR "dsp deps: GNU patch is required to apply the noise-c patches")
        endif()
        file(REMOVE_RECURSE "${dst_root}")
        file(MAKE_DIRECTORY "${dst}")
        file(COPY "${src}/" DESTINATION "${dst}" PATTERN ".git" EXCLUDE)
        foreach(p IN LISTS DSP_NOISE_C_PATCHES)
            execute_process(COMMAND "${DSP_PATCH_EXECUTABLE}" -p1 --forward --batch --fuzz=0
                                    --no-backup-if-mismatch -r - -i "${DSP_NOISE_C_MODULE_DIR}/patches/${p}"
                            WORKING_DIRECTORY "${dst}" RESULT_VARIABLE rc OUTPUT_VARIABLE out
                            ERROR_VARIABLE out)
            if(NOT rc EQUAL 0)
                message(FATAL_ERROR "dsp deps: noise-c patch ${p} does not apply cleanly:\n${out}")
            endif()
        endforeach()
        file(WRITE "${stamp_file}" "${stamp}")
    endif()
    file(SHA256 "${dst}/src/protocol/patterns.c" h1)
    file(SHA256 "${dst}/CMakeLists.txt" h2)
    if(NOT h1 STREQUAL DSP_NOISE_C_PATCHED_PATTERNS_SHA256 OR
       NOT h2 STREQUAL DSP_NOISE_C_PATCHED_CMAKELISTS_SHA256)
        message(FATAL_ERROR "dsp deps: patched noise-c differs from the pinned result "
                "(patterns.c ${h1}, CMakeLists.txt ${h2})")
    endif()
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${DSP_NOISE_C_MODULE_DIR}/noise_c_deps.cmake")
    endif()
    set(${out_dir} "${dst_root}" PARENT_SCOPE)
endfunction()

# Host: libsodium fork (pinned commit + its upstream submodule + the fork's own patches) and the
# patched noise-c, as CMake targets "sodium" and "noise_c". Also "dsp_sodium_hkdf": libsodium's
# own HKDF-SHA256 and HMAC-SHA256 sources from the same pinned tree (absent from the fork's
# curated source list), used only by the host tests as the cross-check provider (the provider
# the Android app uses, decision C1). Nothing in the library is modified.
function(dsp_noise_c_host)
    _dsp_deps_cache(cache)
    set(sod "${cache}/libsodium-${DSP_SODIUM_FORK_COMMIT}")
    _dsp_git_checkout("${DSP_SODIUM_FORK_URL}" "${DSP_SODIUM_FORK_COMMIT}" "${sod}")
    find_package(Git REQUIRED)
    if(NOT EXISTS "${sod}/.dsp_patched")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${sod}" submodule update -q --init --depth 1 libsodium
                        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "dsp deps: libsodium submodule fetch failed:\n${out}")
        endif()
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${sod}/libsodium" rev-parse HEAD
                    OUTPUT_VARIABLE sub OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT sub STREQUAL DSP_SODIUM_UPSTREAM_COMMIT)
        message(FATAL_ERROR "dsp deps: libsodium submodule at ${sub}, pinned ${DSP_SODIUM_UPSTREAM_COMMIT}")
    endif()
    if(NOT EXISTS "${sod}/.dsp_patched")
        execute_process(COMMAND sh patches/apply.sh WORKING_DIRECTORY "${sod}"
                        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "dsp deps: the libsodium fork's patches do not apply:\n${out}")
        endif()
        file(WRITE "${sod}/.dsp_patched" "${DSP_SODIUM_FORK_COMMIT}\n")
    endif()
    dsp_noise_c_prepare("${CMAKE_BINARY_DIR}/dsp_noise_c" nc_root)
    # Third-party code: no project warnings-as-errors; sanitizers stay on (global options).
    add_subdirectory("${sod}" "${CMAKE_BINARY_DIR}/dsp_libsodium" EXCLUDE_FROM_ALL)
    # libsodium's host sources use POSIX/GNU declarations (syscall, getrandom): never build them
    # in strict ISO mode, where those would be implicitly declared.
    set_target_properties(sodium PROPERTIES C_EXTENSIONS ON)
    add_subdirectory("${nc_root}/esphome__noise-c" "${CMAKE_BINARY_DIR}/dsp_noise_c_build" EXCLUDE_FROM_ALL)
    set(ls "${sod}/libsodium/src/libsodium")
    add_library(dsp_sodium_hkdf STATIC
        "${ls}/crypto_kdf/hkdf/kdf_hkdf_sha256.c"
        "${ls}/crypto_auth/hmacsha256/auth_hmacsha256.c")
    target_include_directories(dsp_sodium_hkdf PRIVATE "${ls}/include/sodium")
    target_compile_definitions(dsp_sodium_hkdf PRIVATE CONFIGURED=1)
    target_link_libraries(dsp_sodium_hkdf PUBLIC sodium)
    set_target_properties(dsp_sodium_hkdf PROPERTIES COMPILE_WARNING_AS_ERROR OFF C_EXTENSIONS ON)
endfunction()
