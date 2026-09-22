cmake_minimum_required(VERSION 3.16)
# T-ETH-Lite is the primary hardware target.  A legacy/recovery W5500 build
# explicitly supplies sdkconfig.w5500.defaults instead.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.t_eth_lite.defaults"
        CACHE STRING "Semicolon-separated sdkconfig defaults files")
endif()

# Real ESP-Matter support is opt-in: only wired in when ESP_MATTER_PATH is
# sourced (via esp-matter's own export.sh) before invoking idf.py. Without
# it, every profile (including the default T-ETH-Lite build) is unaffected
# and matter_adapter falls back to its stub implementation.
if(DEFINED ENV{ESP_MATTER_PATH})
    set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
    set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)
    if(NOT DEFINED ENV{ESP_MATTER_DEVICE_PATH})
        set(ENV{ESP_MATTER_DEVICE_PATH} ${ESP_MATTER_PATH}/device_hal/device/esp32_devkit_c)
    endif()
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    include($ENV{ESP_MATTER_DEVICE_PATH}/esp_matter_device.cmake)
    include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
    set(EXTRA_COMPONENT_DIRS
        "${ESP_MATTER_PATH}/examples/common"
        "${MATTER_SDK_PATH}/config/esp32/components"
        "${ESP_MATTER_PATH}/components"
        "${ESP_MATTER_PATH}/device_hal/device"
        ${extra_components_dirs_append})
else()
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
endif()

project(bacnet_bridge VERSION 0.3.0)

if(DEFINED ENV{ESP_MATTER_PATH})
    idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
    idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
endif()
