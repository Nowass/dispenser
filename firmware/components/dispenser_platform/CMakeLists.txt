# dispenser_platform: ESP-IDF adapters for interfaces declared in dispenser_domain.
# ESP-IDF only (never part of the host test build).
#   M1.3: NVS key-value adapter.
#   M1.4: clock port + continuity evidence, Wi-Fi station driver, SNTP time source.
#   M1.7: HTTPS port (esp_http_client + certificate bundle), factory-MAC device ID,
#         monotonic clock, read-only credentials from the "nvs_sec" partition.
idf_component_register(SRCS "src/dsp_nvs_kv.c"
                            "src/dsp_esp_clock.c"
                            "src/dsp_esp_wifi.c"
                            "src/dsp_esp_sntp.c"
                            "src/dsp_esp_http.c"
                            "src/dsp_esp_device.c"
                       INCLUDE_DIRS "include"
                       REQUIRES dispenser_domain
                       PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event lwip esp_rom
                                     esp_http_client esp-tls mbedtls esp_timer esp_hw_support)
