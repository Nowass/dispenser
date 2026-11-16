set(PLATFORM "${CMAKE_CURRENT_LIST_DIR}/../../../components/dispenser_platform")
idf_component_register(SRCS "nvs_capacity.c" "${PLATFORM}/src/dsp_nvs_kv.c"
                       INCLUDE_DIRS "${PLATFORM}/include"
                       REQUIRES nvs_flash esp_partition dispenser_domain
                       WHOLE_ARCHIVE)
# Precise power-off model: every flash write/erase NVS issues passes through the wrappers in
# nvs_capacity.c, which stop ALL flash modification after the cut point.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_partition_write" "-Wl,--wrap=esp_partition_write_raw"
    "-Wl,--wrap=esp_partition_erase_range")
