# dispenser_platform: ESP-IDF adapters for interfaces declared in dispenser_domain.
# ESP-IDF only (never part of the host test build).
#   M1.3: NVS key-value adapter.
#   M1.4: clock port + continuity evidence, Wi-Fi station driver, SNTP time source.
idf_component_register(SRCS "src/dsp_nvs_kv.c"
                            "src/dsp_esp_clock.c"
                            "src/dsp_esp_wifi.c"
                            "src/dsp_esp_sntp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES dispenser_domain
                       PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event lwip esp_rom)
