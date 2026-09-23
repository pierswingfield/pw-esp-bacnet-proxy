## 2026-09-02T20:28:35Z

Task: Implementation Worker for Milestones M3 & M4
- Single Active Integration Mutual Exclusion (M3)
- T-ETH-Lite Matter Thermostat Proof of Concept (M4)

File Write Ownership:
- firmware/bacnet_bridge/partitions_t_eth_lite.csv
- firmware/bacnet_bridge/sdkconfig.t_eth_lite.defaults
- firmware/bacnet_bridge/sdkconfig.w5500.defaults
- firmware/bacnet_bridge/components/matter_adapter/ (or components/hvac_matter/ or components/hvac_core/)
- firmware/bacnet_bridge/components/hvac_core/
- firmware/bacnet_bridge/main/main.c
- firmware/bacnet_bridge/main/CMakeLists.txt
- tests/

Objectives:
1. Create `firmware/bacnet_bridge/partitions_t_eth_lite.csv` providing enlarged dual 4MB OTA slots (4096K each at 0x30000 and 0x430000), 64K NVS, 24K matter_fctry, and 128K coredump for 16MB flash, while leaving partitions.csv (4MB) for W5500 untouched. Update `sdkconfig.t_eth_lite.defaults` with `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_t_eth_lite.csv"`.
2. Implement Single Active Automation Integration (Mutual Exclusion):
   - Modes: `HVAC_INTEGRATION_NONE` (0), `HVAC_INTEGRATION_MQTT_HOME_ASSISTANT` (1), `HVAC_INTEGRATION_MATTER` (2).
   - Dynamic resource isolation: when Matter or None is active, defer/unallocate all MQTT tasks (`mqtt_command_task`, `mqtt_state_task`), `MqttCommandQueue`, and MQTT client/lwIP sockets (>28KB DRAM saved).
   - When Matter is selected, initialize the Matter adapter. On W5500 legacy builds where Matter is disabled (`CONFIG_ENABLE_ESP_MATTER=n`), log warning and gracefully remain Web-only.
   - Integration mode is persisted in NVS namespace `"nvs_integ"`, key `"mode"`.
3. Implement T-ETH-Lite Single Active-Room Matter Thermostat Endpoint (Cluster 0x0201) PoC:
   - Expose Room A standard attributes:
     - Local Temperature (0x0000) mapped to `hvac_core_get_room_temp(0, ...)` / AV:1101.
     - Occupied Cooling Setpoint (0x0011) / Heating Setpoint (0x0012) mapped to `hvac_core_set_room_setpoint(0, ...)` / AV:1100.
     - System Mode (0x001C) mapped to `hvac_core_set_room_power(0, ...)` / BV:1101.
   - Keep Delta-specific vendor features (boost mode, diagnostics, object browser) strictly in the web dashboard.
4. Verify Compilation & Tests:
   - Source /Users/pierswingfield/esp/esp-idf/export.sh
   - Run `./tools/validate_build_profiles.sh` and ensure both T-ETH-Lite and W5500 compile cleanly with 0 errors.
   - Run `python3 tests/run_e2e_tests.py` and ensure all E2E tests pass.
5. Constraints:
   - `bacnet-object-catalog.json` must remain strictly untouched.
   - DO NOT flash physical firmware.
