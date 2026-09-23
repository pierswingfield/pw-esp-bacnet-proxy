# HVAC Core & Matter Specification Report

## Features Discovered

| # | Category | Feature | Description | Inputs | Outputs | Error Behavior | Discovered Via |
|---|----------|---------|-------------|--------|---------|----------------|----------------|
| 1 | HVAC Room Model | `hvac_room_config_t` | Canonical room definition struct representing an HVAC zone | Room name (char[32]), active flag (bool), 6 BACnet instance IDs (uint32_t) | In-memory configuration struct | Invalid instances fallback to default room layout | `components/hvac_core/include/hvac_core.h:10-19` |
| 2 | HVAC Room Model | `HvacRooms` / `HvacRoomCount` | Global canonical array of room configurations and room count | Index (0..7), `HVAC_CORE_MAX_ROOMS = 8` | Active room configuration data | Out-of-bounds access rejected (`room_idx >= HvacRoomCount`) | `components/hvac_core/hvac_core.c:11-18` |
| 3 | Room BACnet Mapping | Occupied Setpoint | BACnet Analog Value (AV) instance for zone temperature setpoint | Float temperature in °C | AV Present_Value write / read | Clamped to `>= 18.0°C` and `<= 30.0°C` | `bacnet-object-catalog.json:599-603`, `main.c:1122-1129` |
| 4 | Room BACnet Mapping | Zone Temperature | BACnet Analog Value (AV) instance for measured zone sensor | Read request | Float temperature in °C | Tag mismatch log error, returns false | `bacnet-object-catalog.json:604-609`, `main.c:980-1000` |
| 5 | Room BACnet Mapping | Zone Run / Power | BACnet Binary Value (BV) instance for room run status | Boolean (true=on, false=off) | BV Present_Value (Enumerated: 1 or 0) | Mutex timeout after 6000ms | `bacnet-object-catalog.json:1462-1467`, `main.c:1023-1069` |
| 6 | Room BACnet Mapping | Supply Air Temp | BACnet Analog Value (AV) instance for duct supply temperature | Read request | Float temperature in °C | Tag validation error handling | `bacnet-object-catalog.json:610-615`, `main.c:4801-4817` |
| 7 | Room BACnet Mapping | Required Thermal Output | BACnet Analog Value (AV) instance for demand % | Read request | Float percentage (0.0..100.0%) | Tag validation error handling | `bacnet-object-catalog.json:628-633`, `main.c:4801-4817` |
| 8 | Room BACnet Mapping | Current Thermal Output | BACnet Analog Value (AV) instance for actual output % | Read request | Float percentage (0.0..100.0%) | Tag validation error handling | `bacnet-object-catalog.json:634-639`, `main.c:6229-6265` |
| 9 | System Master Controls | BMS Run Signal & Status | Master system power write (BV:13) and readback (BV:1) | Boolean power command | System Run state (ON/OFF) | Write fails if BACnet not ready | `main.c:263-264, 1138-1142` |
| 10 | System Master Controls | Boost Mode Control | FCU Operating Mode (MSV:1) for Auto (1), Heating (4), Cooling (5) | Multi-state integer | MSV Present_Value update + timer deadline | Out-of-range mode ignored | `main.c:265-276, 6290-6324` |
| 11 | NVS Persistence | Room Config Schema | NVS namespace `"nvs_rooms"` storing `count` and `r{i}_*` properties | Room properties across all active rooms | Persisted in NVS partition | Non-existent keys fall back to hardcoded defaults | `components/hvac_core/hvac_core.c:7-89` |
| 12 | NVS Persistence | Integration Mode Schema | NVS namespace `"nvs_integ"`, key `"mode"` | Enum value (0, 1, 2) | Stored `hvac_integration_kind_t` | Out-of-bounds integer defaults to MQTT (1) | `components/hvac_core/hvac_core.c:91-133` |
| 13 | Integration Selection | None Mode (`HVAC_INTEGRATION_NONE`) | Web dashboard only mode | User selection `0` | MQTT tasks unallocated, Matter unallocated | N/A | `components/hvac_core/include/hvac_core.h:22`, `docs/INTEGRATION_MODULES.md:24` |
| 14 | Integration Selection | MQTT / HA Mode (`HVAC_INTEGRATION_MQTT_HOME_ASSISTANT`) | MQTT client with Home Assistant auto-discovery | User selection `1` | `mqtt_command_task`, `mqtt_state_task`, client active | Broker connection retries & watchdog | `components/hvac_core/include/hvac_core.h:23`, `main.c:6510-6539` |
| 15 | Integration Selection | Matter Mode (`HVAC_INTEGRATION_MATTER`) | Matter-over-Wi-Fi Thermostat endpoint | User selection `2` | Matter data model active, MQTT deferred | Falls back / disabled on W5500 build | `components/hvac_core/include/hvac_core.h:24`, `docs/MATTER_INTEGRATION_ASSESSMENT_AND_PLAN.md:10-70` |
| 16 | Mutual Exclusion | Deferred MQTT Allocations | Zero MQTT memory consumption when Matter or None is active | Mode check `mqtt_integration_selected()` | `mqtt_app_start()` exits early; tasks not spawned | Logs deferred state and frees heap | `main.c:3506-3510, 6510-6515, 6941-6943` |
| 17 | Matter Cluster | Thermostat LocalTemperature | Standard Matter Thermostat attribute 0x0000 | Measured temp from `temperature_instance` | `int16_t` in 0.01 °C units | Null/error if BACnet read fails | `docs/MATTER_INTEGRATION_ASSESSMENT_AND_PLAN.md:58-65` |
| 18 | Matter Cluster | Thermostat Occupied Setpoint | Standard Matter Thermostat attributes 0x0011 (Cool) / 0x0012 (Heat) | Setpoint float from `setpoint_instance` | `int16_t` in 0.01 °C units | Clamped to [18.0°C, 30.0°C] | `docs/MATTER_INTEGRATION_ASSESSMENT_AND_PLAN.md:58-65` |
| 19 | Matter Cluster | Thermostat SystemMode | Standard Matter Thermostat attribute 0x001C | Power state from `power_instance` | `enum8` (0=Off, 1=Auto, 3=Cool, 4=Heat) | Unsupported modes reject write | `docs/MATTER_INTEGRATION_ASSESSMENT_AND_PLAN.md:58-65` |
| 20 | Vendor Confinement | Web/MQTT Feature Isolation | Confinement of boost, object browser, diagnostics to Web/MQTT | Web HTTP / MQTT payloads | Web UI JSON / MQTT topic outputs | Non-standard Matter clusters avoided | `docs/MATTER_INTEGRATION_ASSESSMENT_AND_PLAN.md:66-70` |

## Edge Cases

| # | Feature | Input | Observed Behavior |
|---|---------|-------|-------------------|
| 1 | Setpoint Limit Enforcement | Request setpoint < 18.0°C (e.g. 15.0°C) | Clamped to `MIN_SETPOINT_C` (18.0°C) before writing BACnet Analog Value. |
| 2 | Setpoint High Limit Enforcement | Request setpoint > 30.0°C (e.g. 35.0°C) | Clamped to 30.0°C in MQTT command handler. |
| 3 | Room Index Out-of-Bounds | API or MQTT command targeting `room_idx >= HvacRoomCount` | Operation rejected with `false` return without executing BACnet transactions. |
| 4 | Unconfigured / Blank Integration Mode in NVS | Clean NVS flash or missing `"mode"` key | Defaults to `HVAC_INTEGRATION_MQTT_HOME_ASSISTANT` (1) for 100% backward compatibility. |
| 5 | Matter Selected on W5500 Profile | Runtime mode set to `HVAC_INTEGRATION_MATTER` on W5500 4MB build | W5500 has no Matter stack compiled (`CONFIG_ENABLE_ESP_MATTER=n`); logs warning and remains Web-only. |
| 6 | Ethernet Link Disconnected During Web Polling | HTTP `GET /api/status` or `/api/health` when `!EthConnected` | Returns JSON `{"bacnet_available":false,"bacnet_reason":"Ethernet link down"}` immediately without blocking on 6-second timeouts. |
| 7 | BACnet Mutex Timeout | BACnet transaction blocked > 6000ms | Logs warning `"BACnet busy, timed out waiting for mutex"`, releases mutex cleanly, returns `false`. |
| 8 | Inactive Room Handling | Room marked `active = false` | Excluded from MQTT discovery, periodic telemetry polling, and Matter endpoint generation. |

---

# 5-Component Handoff Report

## 1. Observation

### Codebase and Architecture State
- **Canonical HVAC Room Model**:
  - Defined in `firmware/bacnet_bridge/components/hvac_core/include/hvac_core.h`:
    - `HVAC_CORE_MAX_ROOMS = 8`.
    - Struct `hvac_room_config_t`: `name[32]`, `active`, `setpoint_instance`, `temperature_instance`, `power_instance`, `supply_air_instance`, `required_output_instance`, `current_output_instance`.
    - `HvacRooms` array and `HvacRoomCount` (default 5 rooms).
  - Defaults in `firmware/bacnet_bridge/components/hvac_core/hvac_core.c:11-27`:
    - Room A: Setpoint AV:1100, Temp AV:1101, Power BV:1101, Supply Air AV:1102, Req Output AV:1105, Cur Output AV:1106.
    - Room B: Setpoint AV:1200, Temp AV:1201, Power BV:1201, Supply Air AV:1202, Req Output AV:1205, Cur Output AV:1206.
    - Room C (base 1300), Room D (base 1400), Room E (base 1500) configured with `active=false`.
- **NVS Schema**:
  - `nvs_rooms`: `count` (uint8_t), `r{i}_name` (string), `r{i}_act` (uint8_t), `r{i}_sp` (uint32_t), `r{i}_temp` (uint32_t), `r{i}_pwr` (uint32_t), `r{i}_sa` (uint32_t), `r{i}_req` (uint32_t), `r{i}_cur` (uint32_t).
  - `nvs_integ`: `mode` (uint8_t: 0=None, 1=MQTT, 2=Matter). Default on uninitialized NVS is `HVAC_INTEGRATION_MQTT_HOME_ASSISTANT`.
- **Semantic Operations & BACnet Access**:
  - `main.c:1122-1142` currently defines:
    - `hvac_command_room_setpoint(size_t room_idx, float requested, float *applied)`
    - `hvac_command_room_power(size_t room_idx, bool on)`
    - `hvac_command_system_power(bool on)`
  - These directly call synchronous BACnet helpers (`write_real_property`, `write_bool_property`) which acquire `BacnetMutex` with a 6-second timeout (`main.c:1006, 1053`).
- **Single Active Automation Integration & Mutual Exclusion**:
  - `main.c:3506-3509`:
    ```c
    static bool mqtt_integration_selected(void) {
        return hvac_core_integration_get() == HVAC_INTEGRATION_MQTT_HOME_ASSISTANT;
    }
    ```
  - `main.c:6512-6515`:
    ```c
    if (!mqtt_integration_selected()) {
        ESP_LOGI(TAG_WIFI, "MQTT integration not selected - resources deferred");
        return;
    }
    ```
  - `main.c:6941-6943`: `mqtt_state_task` is only spawned if `mqtt_integration_selected()` is true.
  - Deferred resources when Matter or None is active:
    - FreeRTOS Task Stacks: `mqtt_command_task` (4,096 B) + `mqtt_state_task` (16,384 B) = 20,480 B.
    - FreeRTOS Queue: `MqttCommandQueue` (8 items * sizeof(mqtt_command_t) = 1,152 B).
    - `MqttClient` struct & lwIP network sockets/TLS context = ~6,000-8,000 B.
    - Total DRAM saved: >28 KB internal DRAM.
- **Matter Thermostat Endpoint & Dependencies**:
  - Target Board: T-ETH-Lite (ESP32-WROVER-E, 16MB flash, 8MB PSRAM).
  - Legacy Board: W5500 (ESP32-WROOM, 4MB flash, 0 PSRAM).
  - Matter Cluster ID 0x0201 (Thermostat) mappings:
    - `LocalTemperature` (0x0000): AV:1101 / AV:1201 mapped to `int16_t` (0.01 °C).
    - `OccupiedCoolingSetpoint` (0x0011) / `OccupiedHeatingSetpoint` (0x0012): AV:1100 / AV:1200 mapped to `int16_t` (0.01 °C).
    - `SystemMode` (0x001C): BV:1101 / BV:1201 mapped to `enum8` (0=Off, 1=Auto/Cool/Heat).
  - Vendor features (Boost MSV:1, Object Browser / Catalog, Diagnostics Health Metrics, System Power BV:13/BV:1) are explicitly confined to Web Dashboard & MQTT.
  - W5500 dependency isolation: `CONFIG_ENABLE_ESP_MATTER` disabled on W5500 builds (`sdkconfig.w5500.defaults`), ensuring zero Matter / BLE symbols in the 4MB binary.

## 2. Logic Chain

1. **Single Source of Truth**:
   - In a multi-transport bridge (Web, MQTT, Matter), having presentation layers independently map BACnet objects or maintain room state causes configuration drift, out-of-sync setpoints, and duplicate NVS storage.
   - Centralizing all room configurations, active states, NVS serialization/deserialization, and semantic actions in `components/hvac_core` guarantees consistent state across Web, MQTT, and Matter.
2. **Mutual Exclusion Requirement**:
   - The ESP32 internal SRAM is constrained (even on WROVER-E with PSRAM, DMA and task stacks consume internal DRAM).
   - Simultaneously running full MQTT client buffers, two MQTT FreeRTOS tasks (20.5 KB stacks), and the full Matter/OpenThread data model + BLE stack risks DRAM exhaustion and socket starvation.
   - Enforcing strict mutual exclusion (`HVAC_INTEGRATION_NONE`, `HVAC_INTEGRATION_MQTT_HOME_ASSISTANT`, `HVAC_INTEGRATION_MATTER`) guarantees that inactive transport allocations remain 0 bytes.
3. **Vendor Confinement to Web UI**:
   - Matter's standard Thermostat cluster does not define manufacturer-specific HVAC boost countdown timers or 480-point BACnet object browser catalogs.
   - Attempting to inject proprietary vendor clusters into Matter causes Apple Home and Google Home to hide or reject the accessory controls.
   - Confining boost and diagnostics to the local Web UI (port 80) preserves standard Matter compliance while maintaining full technician capability.
4. **Decoupling via BACnet Worker Queue**:
   - Current `hvac_command_*` functions in `main.c` block the calling thread (HTTP server worker or MQTT command task) on `BacnetMutex` for up to 6000ms.
   - Transitioning `hvac_core` semantic operations to enqueue bounded requests to the dedicated BACnet worker queue eliminates thread contention and re-entrancy bugs without altering Web/MQTT/Matter interfaces.

## 3. Caveats

1. **Hardware Flashing**: Per project safety rules, no physical flashing was performed. Specification analysis was performed via code inspection, SDK configuration analysis, and memory budgeting.
2. **Catalog Immutability**: `bacnet-object-catalog.json` remains completely unmodified as required.
3. **Matter Multi-Room Endpoint Scaling**: The initial Matter proof-of-concept exposes the primary active room (Room A) as Thermostat Endpoint 1. Multi-room endpoint scaling is planned for subsequent iterations after single-room resource gates pass.

## 4. Conclusion

- `components/hvac_core` is fully specified as the Single Source of Truth for room models, NVS persistence (`nvs_rooms`, `nvs_integ`), and semantic operations.
- Single Active Automation Integration is cleanly gated via `hvac_core_integration_get()` with complete mutual exclusion: MQTT stacks/queues are unallocated when Matter is active.
- Matter Thermostat endpoint mapping conforms to CSA Matter Cluster 0x0201 specifications, with Delta-specific features properly confined to Web UI.
- Dual-profile build integrity is maintained: T-ETH-Lite targets 16MB flash with enlarged OTA partitions for Matter, while W5500 remains a 4MB legacy profile with zero Matter dependencies.

## 5. Verification Method

1. **Dual Build Profile Verification**:
   ```bash
   source /Users/pierswingfield/esp/esp-idf/export.sh
   ./tools/validate_build_profiles.sh
   ```
2. **T-ETH-Lite Build Command**:
   ```bash
   cd firmware/bacnet_bridge && idf.py -B build-t-eth -DSDKCONFIG=build-t-eth/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.t_eth_lite.defaults" build
   ```
3. **Catalog Untouched Verification**:
   ```bash
   git status --porcelain bacnet-object-catalog.json
   ```
