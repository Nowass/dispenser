# dispenser_platform: ESP-IDF adapters for interfaces declared in dispenser_domain.
# ESP-IDF only (never part of the host test build).
#   M1.3: NVS key-value adapter.
#   M1.4: clock port + continuity evidence, Wi-Fi station driver, SNTP time source.
#   M1.7: HTTPS port (esp_http_client + certificate bundle), factory-MAC device ID,
#         monotonic clock, read-only credentials from the "nvs_sec" partition.
#   M1.9c: power port (wake cause, RTC-retained block, deep sleep, fail-safe), queue lock,
#          simulated pump (no GPIO), time-continuity port; pure logic in dsp_platform_logic.c
#          (also compiled by the host tests).
#   Bench: GPIO pump control signal (dsp_pump_gpio pure, host-tested; dsp_esp_pump_gpio compiles
#          to nothing unless CONFIG_DSP_PUMP_GPIO).
#   Battery telemetry: ADC oneshot adapter (dsp_esp_battery; returns no port unless
#          CONFIG_DSP_BATTERY_ADC_GPIO >= 0), pure logic in dsp_platform_logic.c.
#   Firmware update (OTA): esp_https_ota adapter, only with CONFIG_DSP_OTA (docs/FIRMWARE_UPDATE.md).
#   M1.10d.1: BLE byte transport (pure, host-tested) and the NimBLE adapter, only with
#             CONFIG_DSP_BLE_TRANSPORT (build-only until M1.10c.2b and M1.10d.2).
set(dsp_ble_srcs "")
if(CONFIG_DSP_BLE_TRANSPORT)
    set(dsp_ble_srcs "src/dsp_ble_transport.c" "src/dsp_esp_ble.c")
endif()
idf_component_register(SRCS ${dsp_ble_srcs}
                            "src/dsp_nvs_kv.c"
                            "src/dsp_platform_logic.c"
                            "src/dsp_esp_power.c"
                            "src/dsp_esp_sync.c"
                            "src/dsp_esp_pump_sim.c"
                            "src/dsp_pump_gpio.c"
                            "src/dsp_esp_pump_gpio.c"
                            "src/dsp_esp_clock.c"
                            "src/dsp_esp_wifi.c"
                            "src/dsp_esp_sntp.c"
                            "src/dsp_esp_http.c"
                            "src/dsp_esp_device.c"
                            "src/dsp_esp_rng.c"
                            "src/dsp_esp_battery.c"
                            "src/dsp_esp_ota.c"
                       INCLUDE_DIRS "include"
                       REQUIRES dispenser_domain
                       PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event lwip esp_rom
                                     esp_http_client esp-tls mbedtls esp_timer esp_hw_support
                                     esp_system freertos log esphome__libsodium bt esp_driver_gpio
                                     esp_adc esp_https_ota app_update bootloader_support
                                     esp_partition)
# M1.10c.2a: the RNG adapter asks the Bluetooth controller whether the RF entropy source runs.
if(CONFIG_BT_ENABLED)
    idf_component_get_property(_dsp_bt_lib bt COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${_dsp_bt_lib})
endif()
