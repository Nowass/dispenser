# dispenser_platform: ESP-IDF adapters for interfaces declared in dispenser_domain.
# ESP-IDF only (never part of the host test build). M1.3: NVS key-value adapter.
idf_component_register(SRCS "src/dsp_nvs_kv.c"
                       INCLUDE_DIRS "include"
                       REQUIRES dispenser_domain
                       PRIV_REQUIRES nvs_flash)
