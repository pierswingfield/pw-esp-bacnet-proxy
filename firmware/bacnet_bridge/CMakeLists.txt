cmake_minimum_required(VERSION 3.16)
# T-ETH-Lite is the primary hardware target.  A legacy/recovery W5500 build
# explicitly supplies sdkconfig.w5500.defaults instead.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.t_eth_lite.defaults"
        CACHE STRING "Semicolon-separated sdkconfig defaults files")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bacnet_bridge)
