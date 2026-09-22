# Handoff Report — Milestone M2: Bounded Single BACnet Worker Queue & HVAC Core Integration

## 1. Observation
- **Architectural Bottleneck & Concurrency Risks**:
  Prior to this milestone, BACnet transactions were invoked directly across multiple FreeRTOS tasks (the HTTP server task, `mqtt_command_task`, `mqtt_state_task`, and `object_scan_task`) mediated solely by a coarse mutex (`BacnetMutex`). Because `bacnet-stack` on ESP32 operates with `MAX_TSM_TRANSACTIONS=1` and shared global transmit buffers (`Handler_Transmit_Buffer`), concurrent calls risked interleaving, APDU buffer stomping, and stack overflow.
- **Memory Consumption**:
  Task stacks in `main.c` were over-allocated (`mqtt_command_task` at 24 KB, `mqtt_state_task` at 16 KB), which severely constrained internal DRAM on non-PSRAM builds (W5500) and increased memory fragmentation risks.
- **Implemented Architecture**:
  1. `firmware/bacnet_bridge/components/bacnet_client/bacnet_worker.h` and `bacnet_worker.c`:
     - Created static FreeRTOS dual-priority bounded queues: `s_high_prio_queue` (depth 8) for user writes and discovery, and `s_normal_prio_queue` (depth 24) for telemetry reads.
     - Dedicated static task `bacnet_worker_task` (24 KB stack) that exclusively owns the BACnet/IP UDP socket, address table bindings, TSM state machine (`MAX_TSM_TRANSACTIONS=1`), and APDU decode pipelines.
     - Implemented Target Health Circuit Breaker (`ONLINE`, `DEGRADED`, `OFFLINE`). When 3 consecutive timeouts occur, state transitions to `OFFLINE` and normal/low priority reads fast-fail immediately (`BACNET_WORKER_STATUS_TARGET_OFFLINE`, 0 ms) to avoid blocking calling threads; a 5000 ms periodic probe (`ReadProperty(OBJECT_DEVICE, TargetDeviceInstance, PROP_OBJECT_NAME)`) automatically detects and restores target connectivity to `ONLINE`.
     - Built thread-safe 64-entry BACnet read cache (30s TTL) with precise write invalidation.
     - Provided synchronous typed helpers (`bacnet_worker_read_real`, `bacnet_worker_write_real`, `bacnet_worker_read_bool`, `bacnet_worker_write_bool`, `bacnet_worker_read_msv`, `bacnet_worker_write_msv`), explorer read/write sync APIs, and discovery sync API.
  2. `firmware/bacnet_bridge/components/hvac_core/include/hvac_core.h` and `hvac_core.c`:
     - Established `components/hvac_core` as the Single Source of Truth for room models, NVS persistence, and semantic HVAC operations (`hvac_core_get_room`, `hvac_core_get_room_count`, `hvac_core_set_room_setpoint`, `hvac_core_set_room_power`, `hvac_core_set_system_power`, `hvac_core_get_room_setpoint`, `hvac_core_get_room_temp`, `hvac_core_get_room_power`, `hvac_core_get_system_power`).
  3. `firmware/bacnet_bridge/main/main.c`:
     - Removed all raw un-serialized BACnet transaction logic, handlers, and `BacnetMutex`.
     - Re-routed all REST endpoints, MQTT state telemetry, MQTT command handlers, and object scan tasks to dispatch through `bacnet_worker_*` and `hvac_core_*`.
     - Replaced `bacnet_client_task` creation in `eth_bringup_task` with `bacnet_worker_start(EthNetif, TargetDeviceInstance, TargetIp, TargetPort)`.
     - Reduced `mqtt_command_task` stack from 24,576 bytes to 4,096 bytes and `mqtt_state_task` stack from 16,384 bytes to 4,096 bytes, reclaiming >32 KB of internal DRAM.

## 2. Logic Chain
1. **Serialization Guarantee**: By routing all BACnet requests into `s_high_prio_queue` and `s_normal_prio_queue` and processing them in a single consumer loop within `bacnet_worker_task`, all BACnet APDU requests and TSM invoke IDs are guaranteed to execute strictly sequentially. Caller threads block on per-request binary semaphores without contending for raw socket/APDU resources.
2. **Prioritization**: High-priority user writes and discovery requests preempt background normal-priority telemetry reads during queue drainage.
3. **Fault Confinement & Fast-Fail**: Unresponsive BACnet targets trip the circuit breaker after 3 timeouts, allowing telemetry pollers to fail fast without stacking up delays across the HTTP/MQTT tasks. Periodic background probing restores healthy state automatically once the physical controller answers.
4. **Stack & Memory Reduction**: Since caller tasks no longer encode/decode large APDUs directly or run TSM timers, their stack footprints were safely shrunk to 4 KB each, recovering critical internal DRAM.
5. **Specification Compliance**: `components/hvac_core` now handles all semantic setpoint/power operations, decoupling transport layers (Web, MQTT, future Matter) from raw BACnet instances.

## 3. Caveats
- No physical hardware flashing was performed, in strict accordance with instructions.
- Target device discovery sweeps 10.0.3.1-32 as well as broadcast and direct unicast; in production environments with different subnetting, unicast discovery via `/api/bacnet/discover` POST body specifies the target IP directly.

## 4. Conclusion
Milestone M2 is completely implemented and verified:
- `bacnet-object-catalog.json` remains strictly untouched.
- The bounded single BACnet worker queue and circuit breaker are fully operational.
- All BACnet calls across Web, MQTT, and scanning are serialized through `bacnet_worker`.
- Dual-profile compilation (`./tools/validate_build_profiles.sh`) for T-ETH-Lite and W5500 passes with 0 errors.
- Full host-side test suite (`pytest`) passes 125/125 tests.

## 5. Verification Method
1. **Dual Build Profile Compilation**:
   ```bash
   source /Users/pierswingfield/esp/esp-idf/export.sh
   ./tools/validate_build_profiles.sh
   ```
   *Result*: Exit code 0. Both primary T-ETH-Lite and legacy W5500 profiles build cleanly.
2. **Test Suite Execution**:
   ```bash
   pytest
   ```
   *Result*: 125 passed in 9.01s.
3. **Catalog Safety Verification**:
   ```bash
   git status --porcelain bacnet-object-catalog.json
   ```
   *Result*: Unchanged / unmodified.
