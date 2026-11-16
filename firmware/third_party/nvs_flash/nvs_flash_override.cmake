# Project-scoped, patched copy of ESP-IDF's nvs_flash component (see README.md).
#
# The ESP-IDF installation is never modified. At configure time the pinned upstream component
# is verified (SHA-256 manifest of every source file, and the ESP-IDF git commit when IDF_PATH
# is a git checkout), copied into the build directory and patched with patches/*.patch in
# order (GNU patch, zero fuzz); the patched result is verified against a pinned hash. Any
# mismatch stops the configuration with an explicit error: an ESP-IDF update needs a
# deliberate review of this directory.
#
# Project use (before project()):
#   include(${CMAKE_CURRENT_LIST_DIR}/third_party/nvs_flash/nvs_flash_override.cmake)
#   dsp_nvs_flash_override("${CMAKE_BINARY_DIR}/dsp_nvs_flash" DSP_NVS_FLASH_DIR)
#   list(APPEND EXTRA_COMPONENT_DIRS "${DSP_NVS_FLASH_DIR}")
#   ...project()...
#   dsp_nvs_flash_check_selected("${DSP_NVS_FLASH_DIR}")
#
# Script use (test tools):  cmake -DDST=<dir> -P nvs_flash_override.cmake
#   prints the path of the patched component (<dir>/nvs_flash).

set(DSP_NVS_IDF_VERSION "v6.1")
set(DSP_NVS_IDF_COMMIT "fff9895c82d744c7237be8847347bdd1b07c6643")
# SHA-256 over "<sha256>  <relative path>\n" of every file below (sorted): the upstream sources.
set(DSP_NVS_UPSTREAM_MANIFEST_SHA256 "42454d481da5127019a290af2d2c79a554764ad120c6e96f2b06bc3330db25ea")
set(DSP_NVS_UPSTREAM_PAGE_SHA256 "e77a3cb5c9baa6011df2a0cb0053d7f84a003ac1d26d0a7845beadcfaefbfd49")  # src/nvs_page.cpp before patching
set(DSP_NVS_PATCHED_PAGE_SHA256 "a159b269ad45bbcff0510cb1c8169a65d4ba5ecdfd15e8f2675bd9d38bc29813")    # src/nvs_page.cpp after 0001..0003
set(DSP_NVS_PATCHES
    "0001-last-item-duplicate-check-uses-chunk-index.patch"     # espressif/esp-idf#19212
    "0002-verify-hash-match-before-duplicate-erase.patch"       # espressif/esp-idf#19213
    "0003-half-written-entry-check-reads-every-word.patch"      # espressif/esp-idf#19214
)
set(DSP_NVS_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(_dsp_nvs_manifest src out_hash out_text)
    file(GLOB_RECURSE files RELATIVE "${src}" "${src}/include/*" "${src}/private_include/*"
         "${src}/src/*")
    list(APPEND files "CMakeLists.txt" "Kconfig" "project_include.cmake")
    list(SORT files)
    set(text "")
    foreach(f IN LISTS files)
        if(NOT EXISTS "${src}/${f}")
            message(FATAL_ERROR "dsp_nvs_flash: upstream file missing: ${src}/${f}")
        endif()
        file(SHA256 "${src}/${f}" h)
        string(APPEND text "${h}  ${f}\n")
    endforeach()
    string(SHA256 hash "${text}")
    set(${out_hash} "${hash}" PARENT_SCOPE)
    set(${out_text} "${text}" PARENT_SCOPE)
endfunction()

function(dsp_nvs_flash_override dst_root out_dir)
    if(NOT DEFINED ENV{IDF_PATH} OR "$ENV{IDF_PATH}" STREQUAL "")
        message(FATAL_ERROR "dsp_nvs_flash: IDF_PATH is not set")
    endif()
    set(src "$ENV{IDF_PATH}/components/nvs_flash")
    # 1. Pinned upstream: git commit (when available) and source manifest.
    find_package(Git QUIET)
    if(GIT_FOUND AND EXISTS "$ENV{IDF_PATH}/.git")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "$ENV{IDF_PATH}" rev-parse HEAD
                        OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE rc ERROR_QUIET)
        if(rc EQUAL 0 AND NOT commit STREQUAL DSP_NVS_IDF_COMMIT)
            message(FATAL_ERROR "dsp_nvs_flash: ESP-IDF is at ${commit}, the nvs_flash patches are "
                    "pinned to ${DSP_NVS_IDF_VERSION} (${DSP_NVS_IDF_COMMIT}). Review and re-pin "
                    "firmware/third_party/nvs_flash (README.md) before updating ESP-IDF.")
        endif()
    endif()
    _dsp_nvs_manifest("${src}" manifest_hash manifest_text)
    if(NOT manifest_hash STREQUAL DSP_NVS_UPSTREAM_MANIFEST_SHA256)
        file(WRITE "${dst_root}/upstream-manifest.txt" "${manifest_text}")
        message(FATAL_ERROR "dsp_nvs_flash: the upstream nvs_flash sources in ${src} differ from "
                "the pinned ${DSP_NVS_IDF_VERSION} sources (manifest ${manifest_hash}, expected "
                "${DSP_NVS_UPSTREAM_MANIFEST_SHA256}; listing: ${dst_root}/upstream-manifest.txt). "
                "The patches are not applied to unknown sources; see README.md.")
    endif()
    # 2. Copy and patch, unless an identical result already exists (stamp).
    set(patch_hashes "")
    foreach(p IN LISTS DSP_NVS_PATCHES)
        file(SHA256 "${DSP_NVS_MODULE_DIR}/patches/${p}" h)
        string(APPEND patch_hashes "${h};")
    endforeach()
    string(SHA256 stamp "${manifest_hash};${patch_hashes}")
    set(dst "${dst_root}/nvs_flash")
    set(stamp_file "${dst_root}/stamp.txt")
    set(current "")
    if(EXISTS "${stamp_file}")
        file(READ "${stamp_file}" current)
    endif()
    if(NOT current STREQUAL stamp OR NOT EXISTS "${dst}/src/nvs_page.cpp")
        find_program(DSP_PATCH_EXECUTABLE patch)
        if(NOT DSP_PATCH_EXECUTABLE)
            message(FATAL_ERROR "dsp_nvs_flash: GNU patch is required to apply the nvs_flash patches")
        endif()
        file(REMOVE_RECURSE "${dst_root}")
        file(MAKE_DIRECTORY "${dst_root}")
        file(COPY "${src}" DESTINATION "${dst_root}")
        file(SHA256 "${dst}/src/nvs_page.cpp" h)
        if(NOT h STREQUAL DSP_NVS_UPSTREAM_PAGE_SHA256)
            message(FATAL_ERROR "dsp_nvs_flash: unexpected upstream src/nvs_page.cpp (${h})")
        endif()
        foreach(p IN LISTS DSP_NVS_PATCHES)
            execute_process(COMMAND "${DSP_PATCH_EXECUTABLE}" -p1 --forward --batch --fuzz=0
                                    --no-backup-if-mismatch -r - -i "${DSP_NVS_MODULE_DIR}/patches/${p}"
                            WORKING_DIRECTORY "${dst}" RESULT_VARIABLE rc OUTPUT_VARIABLE out
                            ERROR_VARIABLE out)
            if(NOT rc EQUAL 0)
                message(FATAL_ERROR "dsp_nvs_flash: patch ${p} does not apply cleanly:\n${out}")
            endif()
        endforeach()
        file(SHA256 "${dst}/src/nvs_page.cpp" h)
        if(NOT h STREQUAL DSP_NVS_PATCHED_PAGE_SHA256)
            message(FATAL_ERROR "dsp_nvs_flash: patched src/nvs_page.cpp has hash ${h}, expected "
                    "${DSP_NVS_PATCHED_PAGE_SHA256}")
        endif()
        file(WRITE "${stamp_file}" "${stamp}")
    endif()
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${DSP_NVS_MODULE_DIR}/nvs_flash_override.cmake")
        foreach(p IN LISTS DSP_NVS_PATCHES)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                         "${DSP_NVS_MODULE_DIR}/patches/${p}")
        endforeach()
        message(STATUS "nvs_flash: project-scoped copy of ESP-IDF ${DSP_NVS_IDF_VERSION} with "
                "patches 0001-0003 (${dst})")
    endif()
    set(${out_dir} "${dst}" PARENT_SCOPE)
endfunction()

# After project(): the build must really use the patched copy.
function(dsp_nvs_flash_check_selected expected_dir)
    idf_component_get_property(dir nvs_flash COMPONENT_DIR)
    get_filename_component(dir "${dir}" REALPATH)
    get_filename_component(expected_dir "${expected_dir}" REALPATH)
    if(NOT dir STREQUAL expected_dir)
        message(FATAL_ERROR "dsp_nvs_flash: nvs_flash resolves to ${dir}, not the patched copy "
                "${expected_dir}")
    endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    if(NOT DEFINED DST)
        message(FATAL_ERROR "usage: cmake -DDST=<dir> -P nvs_flash_override.cmake")
    endif()
    dsp_nvs_flash_override("${DST}" dir)
    message("${dir}")
endif()
