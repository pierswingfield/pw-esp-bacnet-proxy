if(DEFINED ENV{ESP_MATTER_PATH})
    # Real ESP-Matter Thermostat endpoint. Only reachable when the top-level
    # CMakeLists.txt has already wired in esp-matter's EXTRA_COMPONENT_DIRS
    # (gated on the same ESP_MATTER_PATH env var), so the esp_matter
    # component actually exists to require here.
    idf_component_register(SRCS "matter_adapter.cpp"
                            INCLUDE_DIRS "include"
                            PRIV_REQUIRES hvac_core esp_matter esp_coex
                            LDFRAGMENTS "matter_extram.lf")
    set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
    target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
else()
    # Default/W5500 builds: no esp-matter dependency, pure stub.
    idf_component_register(SRCS "matter_adapter_stub.c"
                            INCLUDE_DIRS "include"
                            REQUIRES hvac_core esp_timer log)
endif()
