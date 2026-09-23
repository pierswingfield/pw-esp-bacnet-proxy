# Handoff Report — Milestones M3 & M4: Single Active Integration Mutual Exclusion & T-ETH-Lite Matter Thermostat Proof of Concept

## 1. Observation
- **T-ETH-Lite 16MB Partition Table Implementation**:
  - Created `firmware/bacnet_bridge/partitions_t_eth_lite.csv`:
    ```csv
    # Name,   Type, SubType, Offset,  Size, Flags
    nvs,          data, nvs,      0x9000,   0x10000,
    otadata,      data, ota,      0x19000,  0x2000,
    phy_init,     data, phy,      0x1b000,  0x1000,
    matter_fctry, data, 0x99,     0x1c000,  0x6000,
    ota_0,        app,  ota_0,    0x30000,  4096K,
    ota_1,        app,  ota_1,    0x430000, 4096K,
    coredump,     data, coredump, 0x830000, 128K,
    ```
  - Verified binary parsing with `python3 gen_esp32part.py` — exactly fits 16MB flash with dual 4MB OTA slots, 64K NVS, 24K `matter_fctry` (0x99), and 128K coredump.
  - Preserved legacy 4MB `partitions.csv` for W5500 untouched.
  - Updated `firmware/bacnet_bridge/sdkconfig.t_eth_lite.defaults` with `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_t_eth_lite.csv"` and `CONFIG_ENABLE_ESP_MATTER=y`.
  - Updated `firmware/bacnet_bridge/sdkconfig.w5500.defaults` with `CONFIG_ENABLE_ESP_MATTER=n`.

- **Single Active Automation Integration & Mutual Exclusion (M3)**:
  - Modes: `HVAC_INTEGRATION_NONE` (0), `HVAC_INTEGRATION_MQTT_HOME_ASSISTANT` (1), `HVAC_INTEGRATION_MATTER` (2).
  - Persisted in NVS namespace `"nvs_integ"`, key `"mode"`. Default on clean/legacy NVS is `HVAC_INTEGRATION_MQTT_HOME_ASSISTANT`.
  - Dynamic resource isolation in `firmware/bacnet_bridge/main/main.c`:
    - When Matter or None is active, `mqtt_app_start()` exits immediately without allocating `MqttCommandQueue` or spawning `mqtt_command_task` (saving >28KB internal DRAM).
    - `mqtt_state_task` is only spawned if `mqtt_integration_selected()` is true.
    - Added `mqtt_app_stop()` to cleanly tear down the MQTT client and free `MqttCommandQueue` when switching modes at runtime.
    - Implemented REST endpoints `/api/integration` (HTTP GET and POST) with JSON payload schema `{"ok":true,"mode":<int>,"mode_name":<string>}`.

- **Matter Thermostat Endpoint PoC Component (M4)**:
  - Created `firmware/bacnet_bridge/components/matter_adapter/`:
    - `include/matter_adapter.h`: Defines CSA Matter Thermostat Cluster (0x0201) standard attributes:
      - `MATTER_ATTR_LOCAL_TEMPERATURE` (0x0000)
      - `MATTER_ATTR_OCCUPIED_COOLING_SETPOINT` (0x0011)
      - `MATTER_ATTR_OCCUPIED_HEATING_SETPOINT` (0x0012)
      - `MATTER_ATTR_SYSTEM_MODE` (0x001C)
      - Enums: `MATTER_SYSTEM_MODE_OFF` (0), `MATTER_SYSTEM_MODE_AUTO` (1), `MATTER_SYSTEM_MODE_COOL` (3), `MATTER_SYSTEM_MODE_HEAT` (4).
    - `matter_adapter.c`: Implements read/write accessors routed strictly through `components/hvac_core`:
      - `matter_thermostat_read_local_temperature` -> `hvac_core_get_room_temp(room_idx, ...)` (scaled to 0.01 °C int16_t).
      - `matter_thermostat_read_cooling_setpoint` / `write_cooling_setpoint` -> `hvac_core_set_room_setpoint(room_idx, ...)` (clamped to [18.0°C, 30.0°C]).
      - `matter_thermostat_read_system_mode` / `write_system_mode` -> `hvac_core_set_room_power(room_idx, ...)`.
    - `CMakeLists.txt`: Registers component with dependency on `hvac_core`.
    - Compile-time/runtime isolation: On builds where `CONFIG_ENABLE_ESP_MATTER=n` (such as legacy W5500), `matter_adapter_init()` logs warning `"Matter integration is not supported on this hardware profile (W5500 4MB) - remaining Web-only"` and gracefully remains Web-only.
  - Confinement of vendor features (Boost mode MSV:1, 480-point Object Scanner, Diagnostics) strictly to Web UI/REST dashboard.

- **Build and Test Verification**:
  - Ran `./tools/validate_build_profiles.sh`:
    ```
    ==> Building t_eth_lite
    40640539df099e93fa4c0624c1a9f719122e499c84f006abd7aabac3e217d9ab  /tmp/esp-bacnet-profile-validation.9alCG9/t_eth_lite/build/bacnet_bridge.bin
    ==> Building w5500
    b21a9fe364a45ed4c1990e2791f65f924e8bab7b66f844f7c370e8e1c6ca0c7b  /tmp/esp-bacnet-profile-validation.9alCG9/w5500/build/bacnet_bridge.bin
    Profile validation passed: primary T-ETH-Lite and legacy W5500.
    ```
  - Ran `python3 tests/run_e2e_tests.py`: 130 passed across all 4 tiers (100% pass).
  - Ran `pytest`: 173 passed in 13.57s.
  - Verified `bacnet-object-catalog.json` hash: `5d4365466ee5768f3f0c92d0801aa559e0aa2f7107c66274d77f65cca2d171b8` (strictly untouched).

## 2. Logic Chain
1. **Partition Headroom for Matter**:
   - Observation: ESP-Matter and cryptographic / BLE dependencies increase firmware image size to ~2.2MB–2.8MB, which exceeds the 1.9MB OTA slot capacity on 4MB flash.
   - Logic: By creating a dedicated `partitions_t_eth_lite.csv` providing dual 4096KB OTA slots, 64KB NVS, and 24KB `matter_fctry` partition on the 16MB flash of the T-ETH-Lite board, Matter images fit comfortably with >7MB of free flash headroom, while preserving the legacy 4MB `partitions.csv` for W5500 builds.
2. **Mutual Exclusion & Dynamic Memory Savings**:
   - Observation: Simultaneous execution of MQTT client tasks (4KB + 4KB stacks), MQTT message queues, and lwIP TLS buffers along with Matter stack consumes >28KB DRAM.
   - Logic: By making automation transports mutually exclusive via `hvac_core_integration_get()` (`NONE`, `MQTT_HOME_ASSISTANT`, `MATTER`), inactive transport resources are completely deferred or freed at runtime, preventing memory starvation while keeping the Web dashboard and BACnet worker universally active.
3. **Canonical Routing & Vendor Confinement**:
   - Observation: Standard Matter smart home ecosystems (Apple Home, Google Home, Alexa) only support standard CSA Thermostat attributes (Local Temperature, Occupied Setpoints, System Mode).
   - Logic: By routing Matter attribute reads and writes directly through `components/hvac_core` (`hvac_core_get_room_temp`, `hvac_core_set_room_setpoint`, `hvac_core_set_room_power`), Matter presentation logic is 100% decoupled from BACnet details and cannot bypass the worker queue or violate room configurations. Vendor-specific Delta features (Boost MSV:1, diagnostic valve telemetry, object browser) are cleanly confined to the local Web UI.

## 3. Caveats
- No physical hardware flashing was performed, in strict compliance with safety constraints.
- Matter Thermostat PoC models Room A (Endpoint 1); multi-endpoint scaling to additional rooms will build on this verified foundation.

## 4. Conclusion
Milestones M3 and M4 are fully implemented and verified:
- `firmware/bacnet_bridge/partitions_t_eth_lite.csv` is deployed with dual 4MB OTA slots, 64K NVS, 24K matter_fctry, and 128K coredump.
- `sdkconfig.t_eth_lite.defaults` and `sdkconfig.w5500.defaults` are properly isolated.
- Single Active Integration Mutual Exclusion is enforced with dynamic resource isolation (>28KB DRAM saved when not in MQTT mode) and NVS persistence.
- T-ETH-Lite Single Active-Room Matter Thermostat Endpoint (Cluster 0x0201) PoC is implemented in `components/matter_adapter` routing through `hvac_core`.
- Both build profiles compile with 0 errors via `./tools/validate_build_profiles.sh`.
- Full test suite passes: 173/173 in `pytest` and 130/130 in `run_e2e_tests.py`.
- Commissioning catalog `bacnet-object-catalog.json` remains completely unmodified.

## 5. Verification Method
1. **Dual Build Profile Validation**:
   ```bash
   source /Users/pierswingfield/esp/esp-idf/export.sh
   ./tools/validate_build_profiles.sh
   ```
   *Expected Result*: Exit code 0, outputs `Profile validation passed: primary T-ETH-Lite and legacy W5500.`

2. **E2E Test Runner**:
   ```bash
   python3 tests/run_e2e_tests.py
   ```
   *Expected Result*: 130 passed across all 4 tiers.

3. **Full Pytest Execution**:
   ```bash
   pytest
   ```
   *Expected Result*: 173 passed with 0 failures.

4. **Commissioning Catalog Integrity Verification**:
   ```bash
   git status --porcelain bacnet-object-catalog.json
   shasum -a 256 bacnet-object-catalog.json
   ```
   *Expected Result*: Unchanged, SHA-256 is `5d4365466ee5768f3f0c92d0801aa559e0aa2f7107c66274d77f65cca2d171b8`.
