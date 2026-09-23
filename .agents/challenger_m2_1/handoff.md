# Empirical Challenger Handoff Report — Milestone M2

**Milestone**: M2 (Bounded Single BACnet Worker Queue & HVAC Core Integration)  
**Agent**: Challenger 1 (`.agents/challenger_m2_1`)  
**Verdict**: **`APPROVE`**

---

## 1. Observation

### 1.1 Dual-Profile Compilation & Partition Verification
- **Command**: `. /Users/pierswingfield/esp/esp-idf/export.sh && ./tools/validate_build_profiles.sh`
- **Results**:
  - **T-ETH-Lite Profile**: Primary ESP32-WROVER-E profile with internal EMAC + RTL8201 PHY and PSRAM support built cleanly.
  - **W5500 Profile**: Legacy ESP32-WROOM profile with SPI W5500 driver and 4MB partition table built cleanly (`bacnet_bridge.bin` size: `0x164b80` bytes / ~1.46MB; `0x76480` bytes / 25% free headroom in smallest 1900KB app slot).
  - **Exit Code**: `0` (Profile validation passed: primary T-ETH-Lite and legacy W5500).

### 1.2 End-to-End Test Suite Execution
- **Command**: `python3 tests/run_e2e_tests.py`
- **Results**:
  - Tier 1 (Feature Coverage): 51 passed in 4.30s
  - Tier 2 (Boundary & Corner Cases): 51 passed in 3.47s
  - Tier 3 (Cross-Feature Combinations): 18 passed in 1.46s
  - Tier 4 (Real-World Scenarios): 5 passed in 0.64s
  - **Total**: 125 passed, 0 failed, 0 errors in 9.86s (Exit code `0`).

### 1.3 Adversarial Stress & Concurrency Testing
- **Harnesses**:
  - `tests/test_challenger_m2_empirical.py` (34 tests)
  - `tests/test_challenger_m2_stress.py` (9 tests)
- **Command**: `pytest`
- **Total Test Results**: 168 passed in 13.55s.
- **Empirical Stress Observations**:
  1. **Heavy Concurrency (40 Threads, 200 Operations)**:
     - High-priority write queue (`HIGH_QUEUE_DEPTH=8`) and normal read queue (`NORMAL_QUEUE_DEPTH=24`) enforced strict bounded backpressure.
     - Out of 70 writes and 130 reads dispatched simultaneously:
       - 66/70 writes succeeded; 4 writes and 10 reads exceeding queue depth were dropped cleanly with `BACnetStatus.QUEUE_FULL` (`worker.dropped_requests_count = 14`).
       - Re-entrancy violations on socket / TSM: **0**.
       - Max concurrent transactions: **1** (strict serialization guaranteed).
  2. **Priority Preemption**:
     - High-priority write requests (`prio=BACnetPriority.HIGH`) preempted normal-priority background telemetry reads during queue drainage.
  3. **Circuit Breaker State Machine**:
     - 1 timeout -> `BACNET_HEALTH_DEGRADED`
     - 2 timeouts -> `BACNET_HEALTH_DEGRADED`
     - 3 timeouts -> `BACNET_HEALTH_OFFLINE`
     - When `OFFLINE`, normal telemetry reads fast-fail immediately (<1ms) with `BACNET_WORKER_STATUS_TARGET_OFFLINE` without blocking calling threads.
     - Flapping behavior (2 timeouts followed by 1 successful response) resets `consecutive_timeouts` to 0 and maintains `ONLINE`, preventing false trips.
     - Recovery probe (`ReadProperty(OBJECT_DEVICE, TargetDeviceInstance, PROP_OBJECT_NAME)`) automatically detects target availability and restores health to `ONLINE`.
     - High-priority writes bypass the `OFFLINE` fast-fail filter, allowing user commands to attempt transmission directly.
  4. **HVAC Core Clamping & Robustness**:
     - Room setpoints clamp strictly to `[18.0°C - 30.0°C]` across out-of-range floats, subnormals, and extreme values (`+/-inf`).
     - Room index queries out of range (`-1`, `8`, `999`) return `NULL` / `false` without memory corruption.
     - System power writes to `BV:13` and reads back from `BV:1` per specification.
  5. **NVS Resilience**:
     - Out-of-bounds room count (e.g. `255`) safely falls back to default 5 rooms.
     - Invalid integration mode (e.g. `42`) safely falls back to default `MQTT_HOME_ASSISTANT`.

### 1.4 Code Inspection & Architecture Verification
- **File**: `firmware/bacnet_bridge/components/bacnet_client/bacnet_worker.c`:
  - Static memory allocations: `s_worker_stack` (24KB), `s_high_prio_queue_storage` (8 items), `s_normal_prio_queue_storage` (24 items).
  - Single consumer loop `bacnet_worker_task` exclusively owns the BACnet/IP UDP socket, APDU decoding, and TSM invoke IDs.
- **File**: `firmware/bacnet_bridge/components/hvac_core/hvac_core.c`:
  - Canonical Single Source of Truth for room model and semantic HVAC operations.
  - No direct socket access; all operations route through `bacnet_worker_*`.
- **File**: `firmware/bacnet_bridge/main/main.c`:
  - Removed all raw un-serialized BACnet calls and mutexes.
  - Task stack reductions confirmed:
    - `mqtt_command_task`: 24,576 B -> 4,096 B (Line 5616)
    - `mqtt_state_task`: 16,384 B -> 4,096 B (Line 6016)
    - Reclaimed internal DRAM: >32 KB.
- **Catalog Invariance**:
  - `bacnet-object-catalog.json` remains completely unmodified (SHA256: `5d4365466ee5768f3f0c92d0801aa559e0aa2f7107c66274d77f65cca2d171b8`).
- **Hardware Safety**: Zero physical firmware flashing executed.

---

## 2. Logic Chain

1. **Queue Bounds & Serialization**: Because all confirmed BACnet requests enter static FreeRTOS queues and are processed exclusively by `bacnet_worker_task`, multi-threaded socket contention and TSM re-entrancy violations (`MAX_TSM_TRANSACTIONS=1`) are eliminated.
2. **Priority Ordering**: User write commands and discovery requests enqueue with `BACnetPriority.HIGH` and drain before `BACnetPriority.NORMAL` telemetry reads, preventing UI/automation command lag during heavy background polling.
3. **Target Health & Fast-Fail**: The 3-timeout threshold transitioning to `OFFLINE` prevents calling tasks from hanging up to 800ms per dead read. The background probe guarantees automatic self-healing upon controller restoration.
4. **Memory Optimization**: Offloading APDU buffer allocation to the worker stack allowed caller task stacks (`mqtt_command_task`, `mqtt_state_task`) to be reduced to 4KB each, reclaiming >32KB of DRAM and ensuring W5500 4MB profile compatibility.
5. **Architectural Purity**: `components/hvac_core` cleanly abstracts the physical BACnet mapping, ensuring downstream transport adapters (Web, MQTT, future Matter) interact purely via semantic APIs.

---

## 3. Caveats

- Physical microcontroller flashing was intentionally skipped per safety constraints; all tests were verified via dual-profile compilation, hardware architecture audits, and full-fidelity simulator test harnesses.
- Dynamic client discovery targets subnet `10.0.3.1-32` and global broadcast by default; custom IP addresses can be supplied via `/api/bacnet/discover`.

---

## 4. Conclusion

**Verdict: `APPROVE`**

Milestone M2 meets all requirements:
1. BACnet worker queue is bounded (`HIGH=8`, `NORMAL=24`), serialized, prioritized, and thread-safe.
2. Target health circuit breaker fast-fails dead-target telemetry and recovers automatically via background probes.
3. `hvac_core` functions as the canonical Single Source of Truth for room models and semantic operations.
4. Dual build profile validation passes with 0 errors on both T-ETH-Lite and W5500 targets.
5. 100% of E2E tests (125/125) and extended adversarial stress tests (168/168) pass cleanly.
6. `bacnet-object-catalog.json` is untouched, and no physical flashing was executed.

---

## 5. Verification Method

1. **Dual Build Profile Compilation**:
   ```bash
   source /Users/pierswingfield/esp/esp-idf/export.sh
   ./tools/validate_build_profiles.sh
   ```
2. **Complete Test Suite (168 Tests)**:
   ```bash
   pytest
   ```
3. **E2E Structured Runner**:
   ```bash
   python3 tests/run_e2e_tests.py
   ```
4. **Catalog Invariance Check**:
   ```bash
   git status --porcelain bacnet-object-catalog.json
   ```
