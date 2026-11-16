# M1.10a Noise feasibility gate (host and Android NDK builds).
#
# Fetches esphome/noise-c at the pinned commit, applies the Dispenser V2 local patches
# (../patches, unless NOISE_GATE_PATCHES=OFF for the regression check), and lets noise-c's
# own test build fetch its pinned esphome/libsodium (idf_component.yml: 1.10021.12).
# Run everything with ./run_gate.sh; see ../README.md.
cmake_minimum_required(VERSION 3.16)
project(noise_gate C)
enable_testing() # so that ctest at the top level runs noise-c's own unit and vector tests

option(NOISE_GATE_PATCHES "Apply the Dispenser V2 local patches" ON)
set(NOISE_C_COMMIT 62c2fdca7a874ea898761147bb062ca1cde9f36d) # esphome/noise-c v0.1.31

set(_patches
    "${CMAKE_CURRENT_LIST_DIR}/../patches/0001-patterns-bound-base-pattern-search-by-table-size.patch"
    "${CMAKE_CURRENT_LIST_DIR}/../patches/0002-esp-idf-keep-default-feature-switches.patch")
if(NOISE_GATE_PATCHES)
  set(_patch_cmd PATCH_COMMAND git checkout -q -- . COMMAND git apply ${_patches})
else()
  set(_patch_cmd PATCH_COMMAND git checkout -q -- .)
endif()

include(FetchContent)
FetchContent_Declare(noise_c
  GIT_REPOSITORY https://github.com/esphome/noise-c.git
  GIT_TAG ${NOISE_C_COMMIT}
  ${_patch_cmd})
set(NOISE_C_BUILD_TESTS ON CACHE BOOL "Build the upstream unit and vector tests" FORCE)
FetchContent_MakeAvailable(noise_c)

# Probe: in-process self-test (XXpsk3 + IK, transport, failure cases, transcript digest) and
# the cross-process initiator/responder used for interoperability between builds.
add_executable(nprobe nprobe.c)
target_link_libraries(nprobe PRIVATE noise_c sodium)

# Regression test of the patched pattern lookup (all base patterns, modifiers, unknown ids).
add_executable(test_patterns test_patterns.c)
target_link_libraries(test_patterns PRIVATE noise_c sodium)

if(ANDROID)
  # Stand-in for the future JNI library: the probe as a shared object, for size measurement.
  add_library(dspnoise SHARED nprobe.c)
  target_compile_definitions(dspnoise PRIVATE NPROBE_NO_MAIN=1)
  target_link_libraries(dspnoise PRIVATE noise_c sodium)
  target_link_options(dspnoise PRIVATE -Wl,--gc-sections -Wl,--exclude-libs,ALL)
else()
  # Host only: peak heap accounting through linker wrapping.
  add_executable(nprobe_heap nprobe.c heapwrap.c)
  target_link_libraries(nprobe_heap PRIVATE noise_c sodium)
  target_link_options(nprobe_heap PRIVATE
    -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc)
endif()
