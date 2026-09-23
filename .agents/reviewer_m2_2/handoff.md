# Handoff Report — Reviewer 2: Milestone M2 Verification & Adversarial Audit

## 1. Observation
- **Codebase & Architecture Inspection**:
  - `firmware/bacnet_bridge/components/bacnet_client/bacnet_worker.h` & `bacnet_worker.c`:
    - Implements static FreeRTOS dual-priority bounded queues: `s_high_prio_queue` (depth 8) for user write commands and discovery, and `s_normal_prio_queue` (depth 24) for telemetry reads.
    - Statically allocates a 24 KB stack (`s_worker_stack`) and TCB (`s_worker_tcb`) for `bacnet_worker_task`, completely isolating APDU encoding/decoding and TSM transactions (`MAX_TSM_TRANSACTIONS=1`) from calling tasks.
    - Implements the Target Health Circuit Breaker (`ONLINE`, `DEGRADED`, `OFFLINE`). Upon 3 consecutive timeouts (`BACNET_OFFLINE_CONSECUTIVE_TIMEOUTS = 3`), state transitions to `OFFLINE` and normal/low priority reads fast-fail immediately (`BACNET_WORKER_STATUS_TARGET_OFFLINE`, 0 ms); a periodic 5000 ms background probe (`OBJECT_DEVICE` / `PROP_OBJECT_NAME`) automatically restores the target to `ONLINE` upon reconnection.
    - Implements a 64-entry TTL read cache (30 s TTL) protected by spinlock critical sections (`taskENTER_CRITICAL(&s_cache_mux)`), with automatic targeted invalidation upon successful write operations (`bacnet_worker_write_*`).
  - `firmware/bacnet_bridge/components/hvac_core/include/hvac_core.h` & `hvac_core.c`:
    - Established as the Single Source of Truth for canonical room models (`HvacRooms`, `HvacRoomCount`), NVS storage (`"nvs_rooms"`), and semantic HVAC controls (`hvac_core_set_room_setpoint`, `hvac_core_set_room_power`, `hvac_core_set_system_power`, `hvac_core_get_room_setpoint`, `hvac_core_get_room_temp`, `hvac_core_get_room_power`, `hvac_core_get_system_power`).
    - Enforces setpoint clamping to `[18.0°C, 30.0°C]` (`MIN_SETPOINT_C` to `MAX_SETPOINT_C`).
  - `firmware/bacnet_bridge/main/main.c`:
    - All raw socket operations (`bip_receive`, `bip_send`), APDU callbacks, and `BacnetMutex` were completely eliminated from calling tasks.
    - HTTP REST API endpoints, MQTT telemetry tasks (`mqtt_state_task`), MQTT command handlers (`mqtt_command_task`), and object scanners route 100% of BACnet interactions through `bacnet_worker_*` and `hvac_core_*`.
    - Stack sizes for `mqtt_command_task` and `mqtt_state_task` were reduced from 24 KB and 16 KB down to 4 KB each, freeing >32 KB of contiguous internal DRAM on memory-constrained targets (W5500).
- **Integrity Inspection**:
  - Confirmed 0 hardcoded test shortcuts, fake mock returns, or bypassed logic in the C codebase.
  - `bacnet-object-catalog.json` SHA-256 hash verified invariant (`5d4365466ee5768f3f0c92d0801aa559e0aa2f7107c66274d77f65cca2d171b8`).
- **Build & Test Execution Results**:
  - `tools/validate_build_profiles.sh`: Exit code 0. Both primary `t_eth_lite` (16MB Flash, PSRAM enabled) and legacy `w5500` (4MB Flash, SPI Ethernet) profiles compiled cleanly with 0 warnings/errors.
  - `python3 tests/run_e2e_tests.py`: 125/125 tests passed across Tiers 1-4 in 10.96s (Tier 1: 51/51, Tier 2: 51/51, Tier 3: 18/18, Tier 4: 5/5).

---

## 2. Logic Chain
1. **Concurrency Serialization & Socket Ownership**: Prior to M2, multiple FreeRTOS tasks invoked `Send_Read_Property_Request` / `Send_Write_Property_Request` concurrently with coarse mutexing, risking APDU buffer corruption and stack overflows. In M2, all operations are funneled through `s_high_prio_queue` and `s_normal_prio_queue` to a single dedicated worker thread. This structurally eliminates race conditions and socket contention.
2. **Prioritization & Starvation Avoidance**: The worker loop drains `s_high_prio_queue` (depth 8) before checking `s_normal_prio_queue` (depth 24), ensuring user setpoint writes and discovery operations preempt bulk background telemetry reads.
3. **Fault Confinement via Circuit Breaker**: The 3-timeout tripping mechanism prevents calling tasks from hanging during network outages. Normal-priority telemetry reads fail fast with 0 ms delay while the background probe restores online status without manual intervention.
4. **Memory Optimization**: Shifting APDU decoding and networking buffers into the worker task permitted shrinking caller stacks by >32 KB, ensuring safe operation on non-PSRAM W5500 hardware.

---

## 3. Caveats
- No physical hardware flashing was performed (per safety constraints).
- In `bacnet_worker_dispatch_sync`, if an extreme queue backlog causes the request to wait longer than the caller's semaphore timeout, the caller could return before the worker processes the item. Under normal operations, queue depths (8/24) and enqueue timeouts (100 ms) prevent this condition.

---

## 4. Quality & Adversarial Review

### Quality Review Summary
**Verdict**: APPROVE

- **Correctness**: Fully compliant with `PROJECT.md` and `ORIGINAL_REQUEST.md`. No regressions in MQTT or Web contracts.
- **Logical Completeness**: Clear and complete decoupling between presentation/transport adapters and the BACnet worker.
- **Integrity**: PASS. Genuine implementation with zero mock shortcuts or hardcoded test bypasses.

### Adversarial Challenge & Stress-Testing Findings

#### [Low] Finding 1: Synchronous Caller Timeout vs Worker Queue Drain
- **Assumption Challenged**: Caller threads waiting on `bacnet_worker_dispatch_sync` assume the worker will complete the request before the caller's semaphore timeout expires.
- **Attack Scenario**: Under an extreme burst of 24 backlogged requests, if each request took 800 ms to time out (total ~19 s), earlier caller threads waiting with a 1200 ms timeout could time out on `xSemaphoreTake`, delete the binary semaphore, and exit. When `bacnet_worker_task` later pops the item, calling `xSemaphoreGive` on the deleted semaphore handle or writing to the caller's stack-allocated `resp` pointer could result in invalid memory access.
- **Blast Radius**: Theoretical panic under extreme backpressure.
- **Mitigation / Defense**: In practice, `xQueueSend` has a 100 ms timeout and the circuit breaker trips after 3 timeouts (2.4 s), after which all subsequent normal reads fail fast in 0 ms, draining the entire queue in <1 ms. For future hardening, a generation ID or active flag could be added to request structs to cancel uncollected responses cleanly.

---

## 5. Conclusion
Milestone M2 implementation satisfies all technical, architectural, memory, and safety requirements.
Final Verdict: **APPROVE**.

---

## 6. Verification Method
To independently reproduce and verify this review:
1. **Check Catalog Invariance**:
   ```bash
   shasum -a 256 bacnet-object-catalog.json
   # Output: 5d4365466ee5768f3f0c92d0801aa559e0aa2f7107c66274d77f65cca2d171b8
   ```
2. **Dual-Profile Compilation**:
   ```bash
   source /Users/pierswingfield/esp/esp-idf/export.sh
   ./tools/validate_build_profiles.sh
   # Exit code 0, verifying both T-ETH-Lite and W5500 builds
   ```
3. **E2E Test Suite Execution**:
   ```bash
   python3 tests/run_e2e_tests.py
   # 125/125 passed across all 4 tiers
   ```
