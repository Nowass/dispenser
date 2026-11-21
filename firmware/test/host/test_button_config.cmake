# Host check of the MVP wake button configuration (sdkconfig.button.defaults) against the pump
# fragment and the ESP32-C3 wake-pad rules. Run with
#   cmake -P test/host/test_button_config.cmake
# (registered in CTest as test_button_config). No ESP-IDF needed.

get_filename_component(FW "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

function(cfg file key out)
    file(STRINGS "${FW}/${file}" lines REGEX "^${key}=")
    list(LENGTH lines n)
    if(NOT n EQUAL 1)
        message(FATAL_ERROR "${file}: expected exactly one ${key}= line, found ${n}")
    endif()
    string(REGEX REPLACE "^${key}=" "" v "${lines}")
    set(${out} "${v}" PARENT_SCOPE)
endfunction()

cfg(sdkconfig.button.defaults CONFIG_DSP_BUTTON_WAKE_GPIO btn)
cfg(sdkconfig.button.defaults CONFIG_DSP_BUTTON_WAKE_ACTIVE_HIGH high)
cfg(sdkconfig.button.defaults CONFIG_ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS pulls)
cfg(sdkconfig.pumpgpio.defaults CONFIG_DSP_PUMP_GPIO_NUM pump)

set(failures 0)
macro(fail msg)
    message("FAIL: ${msg}")
    math(EXPR failures "${failures} + 1")
endmacro()

if(NOT btn STREQUAL "3")
    fail("MVP wake button must be XIAO D1 = GPIO3 (got ${btn})")
endif()
if(NOT high STREQUAL "n")
    fail("MVP wake button is active low (got ACTIVE_HIGH=${high})")
endif()
if(NOT pulls STREQUAL "y")
    fail("internal sleep pull-up kept as a backup of the external 10 kOhm (got ${pulls})")
endif()
if(btn EQUAL pump)
    fail("button GPIO ${btn} equals the pump control GPIO ${pump}")
endif()
# ESP32-C3 deep-sleep wake pads are GPIO0-5; GPIO2 is a strapping pin.
if(btn LESS 0 OR btn GREATER 5 OR btn EQUAL 2)
    fail("GPIO ${btn} is not a usable wake pad")
endif()
# The default image keeps the button disabled.
file(READ "${FW}/components/dispenser_platform/Kconfig" kc)
if(NOT kc MATCHES "config DSP_BUTTON_WAKE_GPIO\n[^\n]*\n[^\n]*\n    default -1\n")
    fail("Kconfig default of DSP_BUTTON_WAKE_GPIO must stay -1")
endif()
foreach(f sdkconfig.defaults sdkconfig.ble.defaults sdkconfig.wifitime.defaults sdkconfig.pumpgpio.defaults)
    file(STRINGS "${FW}/${f}" hit REGEX "^CONFIG_DSP_BUTTON_WAKE")
    if(NOT hit STREQUAL "")
        fail("${f} must not configure the button (only sdkconfig.button.defaults)")
    endif()
endforeach()

if(failures GREATER 0)
    message(FATAL_ERROR "${failures} button configuration check(s) failed")
endif()
message("button configuration: GPIO${btn} active low, pump GPIO${pump}, internal sleep pull-up kept: OK")
