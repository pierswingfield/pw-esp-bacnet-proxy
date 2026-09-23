# Forensic Audit Report — Milestone M2: Bounded Single BACnet Worker Queue & HVAC Core Integration

**Work Product**: Milestone M2 Implementation (`firmware/bacnet_bridge/components/bacnet_client/bacnet_worker.*`, `firmware/bacnet_bridge/components/hvac_core/*`, `firmware/bacnet_bridge/main/main.c`, `tools/validate_build_profiles.sh`, `tests/`)
**Profile**: General Project (Integrity Forensics)
**Integrity Mode**: Development Mode (from `ORIGINAL_REQUEST.md`)
**Verdict**: **`CLEAN`**

---

## 1. Observation

### A. Source Code Integrity Analysis
1. **`firmware/bacnet_bridge/components/bacnet_client/bacnet_worker.h` & `bacnet_worker.c`**:
   - Genuine implementation of static FreeRTOS dual-priority bounded queues:
     - `s_high_prio_queue`: Capacity 8 (`BACNET_HIGH_PRIO_QUEUE_DEPTH`), statically allocated using `s_high_prio_queue_storage` and `s_high_prio_queue_struct`.
     - `s_normal_prio_queue`: Capacity 24 (`BACNET_NORMAL_PRIO_QUEUE_DEPTH`), statically allocated using `s_normal_prio_queue_storage` and `s_normal_prio_queue_struct`.
   - Dedicated static task `bacnet_worker_task` (24 KB stack) exclusively handles the BACnet UDP socket (`bip_init`, `bip_receive`), TSM state machine (`MAX_TSM_TRANSACTIONS=1`), APDU decoding, and address bindings (`address_bind_request`).
   - Implements Target Health Circuit Breaker state machine (`ONLINE`, `DEGRADED`, `OFFLINE`). When 3 consecutive timeouts occur, state transitions to `OFFLINE` and normal/low priority reads fast-fail immediately (`BACNET_WORKER_STATUS_TARGET_OFFLINE`, 0 ms); background periodic probe every 5000 ms restores `ONLINE` state automatically on response.
   - Implements 64-entry thread-safe read cache (`BACNET_CACHE_ENTRIES = 64`, 30s TTL) protected by spinlock `s_cache_mux` with precise write invalidation.
   - Public typed helpers (`bacnet_worker_read_real`, `bacnet_worker_write_real`, `bacnet_worker_read_bool`, `bacnet_worker_write_bool`, `bacnet_worker_read_msv`, `bacnet_worker_write_msv`) and explorer APIs (`bacnet_worker_explorer_read_sync`, `bacnet_worker_explorer_write_sync`, `bacnet_worker_discover_sync`).

2. **`firmware/bacnet_bridge/components/hvac_core/include/hvac_core.h` & `hvac_core.c`**:
   - Single Source of Truth for canonical room model (`HvacRooms`, `HvacRoomCount`).
   - NVS schema `nvs_rooms` persists room configurations, active statuses, names, and BACnet object instances.
   - NVS schema `nvs_integ:mode` manages runtime integration selection.
   - Semantic HVAC operations (`hvac_core_set_room_setpoint`, `hvac_core_set_room_power`, `hvac_core_set_system_power`, `hvac_core_get_room_setpoint`, `hvac_core_get_room_temp`, `hvac_core_get_room_power`, `hvac_core_get_system_power`) clamp setpoints to `[18.0°C, 30.0°C]` and route directly through `bacnet_worker_*`.

3. **`firmware/bacnet_bridge/main/main.c`**:
   - Removed coarse `BacnetMutex`.
   - All REST API endpoints, MQTT state telemetry, MQTT command handlers, and object scan tasks are re-routed through `bacnet_worker_*` and `hvac_core_*`.
   - Replaced `bacnet_client_task` creation with `bacnet_worker_start(EthNetif, TargetDeviceInstance, TargetIp, TargetPort)`.
   - Task stack allocations reduced: `mqtt_command_task` reduced from 24,576 bytes to 4,096 bytes; `mqtt_state_task` reduced from 16,384 bytes to 4,096 bytes (>32 KB DRAM reclaimed).

### B. Invariance & Safety Checks
1. **Catalog Integrity**:
   - `bacnet-object-catalog.json` is 100% clean and unmodified.
   - SHA-256: `5d4365466ee5768f3f0c92d0801aa559e0aa2f7107c66274d77f65cca2d171b8` (2562 lines, 426 objects).
2. **Firmware Flashing Safety**:
   - Zero hardware flashing commands executed.
   - Tools and validation scripts only compile to temporary directories without invoking `esptool.py write_flash` or `idf.py flash` on connected microcontrollers.

### C. Empirical Build and Test Execution
1. **Dual-Profile Compilation**:
   - Command: `source /Users/pierswingfield/esp/esp-idf/export.sh && ./tools/validate_build_profiles.sh`
   - Primary `t_eth_lite` profile: Built cleanly, ELF and binary created.
   - Legacy `w5500` profile: Built cleanly (`bacnet_bridge.bin` binary size 0x164b80 bytes, 25% free headroom in 1900KB app partition).
   - Exit code: `0`.
2. **Test Suite Verification**:
   - Command: `pytest -v`
   - Result: `125 passed in 10.23s` (100% pass across all 5 test tiers).

---

## 2. Logic Chain

1. **Absence of Prohibited Patterns**:
   - Source code analysis confirmed no hardcoded test return values, dummy stubs, facade implementations, or fake attestations exist.
   - Every worker queue, cache, circuit breaker, NVS persistence, and semantic HVAC function executes genuine logic.
2. **Serialization & Concurrency Safety**:
   - All BACnet transactions across Web, MQTT, and scanner are serialized through `bacnet_worker_task` via FreeRTOS queues `s_high_prio_queue` and `s_normal_prio_queue`. Direct socket access and multi-threaded stack re-entrancy are completely eliminated.
3. **Fault Confinement**:
   - The circuit breaker prevents offline target timeouts from cascading or locking calling tasks.
4. **Constraint Compliance**:
   - The commissioning catalog `bacnet-object-catalog.json` remains completely untouched.
   - Dual profile compilation for T-ETH-Lite and legacy W5500 builds cleanly without errors.
   - No physical firmware flashing was conducted.

---

## 3. Caveats

- In accordance with safety rules, physical flashing to connected microcontrollers was strictly omitted. All behavioral testing was verified via host-side simulation test suites and clean dual-profile ESP-IDF firmware compilation.

---

## 4. Conclusion

**Verdict**: **`CLEAN`**

Milestone M2 strictly adheres to all architectural requirements and constraints:
- Bounded dual-priority BACnet worker queue is fully operational.
- `components/hvac_core` acts as the single source of truth for rooms and semantic controls.
- Memory footprint is optimized (>32 KB DRAM saved by shrinking MQTT task stacks).
- Dual-profile compilation (`./tools/validate_build_profiles.sh`) passes with 0 errors.
- `bacnet-object-catalog.json` is unmodified.
- No physical firmware flashing was performed.

---

## 5. Verification Method

To independently reproduce this forensic audit:

1. **Verify Catalog Invariance**:
   ```bash
   shasum -a 256 bacnet-object-catalog.json
   # Expected: 5d4365466ee5768f3f0c92d0801aa559e0aa2f7107c66274d77f65cca2d171b8
   ```

2. **Verify Dual Profile Build**:
   ```bash
   source /Users/pierswingfield/esp/esp-idf/export.sh
   ./tools/validate_build_profiles.sh
   # Expected: Exit code 0, "Profile validation passed: primary T-ETH-Lite and legacy W5500."
   ```

3. **Verify Full Test Suite**:
   ```bash
   pytest
   # Expected: 125 passed
   ```
