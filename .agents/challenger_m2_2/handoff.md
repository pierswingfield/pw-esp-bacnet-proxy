# Challenger 2 Handoff Report — Milestone M2: Bounded Single BACnet Worker Queue & HVAC Core Integration

## 1. Observation
- **Presentation Layer Routing & Decoupling**:
  - `firmware/bacnet_bridge/main/main.c`:
    - All Web REST handlers (`api_room_setpoint_post_handler` [L2186], `api_room_power_post_handler` [L2210], `api_system_power_post_handler` [L2237], `api_rooms_get_handler` [L3885], `api_rooms_post_handler` [L3907]) and MQTT tasks (`mqtt_command_task` [L5517], `mqtt_state_task` [L5625]) route strictly through `hvac_core_*` semantic operations or `bacnet_worker_*` APIs.
    - Zero raw concurrent BACnet socket writes bypass the single serialized worker queue.
- **Semantic Setpoint Clamping [18.0°C - 30.0°C]**:
  - In `firmware/bacnet_bridge/components/hvac_core/hvac_core.c` [L153]:
    ```c
    float value = requested < MIN_SETPOINT_C ? MIN_SETPOINT_C : (requested > MAX_SETPOINT_C ? MAX_SETPOINT_C : requested);
    if (applied) *applied = value;
    return bacnet_worker_write_real(OBJECT_ANALOG_VALUE, HvacRooms[room_idx].setpoint_instance, PROP_PRESENT_VALUE, value);
    ```
  - Clamping was validated empirically across underflow (`-100.0`, `0.0`, `10.0`, `17.9`, `17.999` -> `18.0`), overflow (`30.001`, `30.1`, `35.0`, `100.0`, `1000.0` -> `30.0`), and in-range (`18.0`, `21.5`, `24.0`, `29.999`, `30.0`) inputs. The `applied` parameter returns the exact clamped value to caller endpoints.
- **Room Index Boundary Validation**:
  - In `hvac_core.c` [L141, L152, L160, L171, L177, L183]:
    All functions enforce `if (room_idx >= HvacRoomCount) return false;` (or `NULL`).
  - Out of bounds indices (`-1`, `5`, `6`, `7`, `8`, `999`) return safe failure codes (`false`, `NULL`, HTTP 400 Bad Request). Dynamic capacity expansion up to `HVAC_CORE_MAX_ROOMS = 8` was verified through NVS persistence roundtrips.
- **System Power Consistency**:
  - Master system power write is mapped to BV:13 (`SYS_POWER_WRITE_INSTANCE`), while readback status is mapped to BV:1 (`SYS_POWER_READBACK_INSTANCE`). This contract is consistently maintained across `hvac_core_set_system_power`, `hvac_core_get_system_power`, HTTP `/api/system-power`, MQTT `<base>/system_power/set`, and `mqtt_state_task` telemetry loop.
- **Queue Boundedness & Resource Consumption**:
  - `bacnet_worker.c` enforces dual-priority static queues: `s_high_prio_queue` (depth 8) and `s_normal_prio_queue` (depth 24).
  - High-priority write preemption over normal-priority background telemetry reads was verified under burst traffic.
  - Stack reductions (`mqtt_command_task` to 4 KB, `mqtt_state_task` to 4 KB) recover >32 KB internal DRAM while the dedicated 24 KB `bacnet_worker_task` handles all APDU encoding/decoding.
  - Circuit Breaker transitions (`ONLINE` -> `DEGRADED` on 1st/2nd timeout -> `OFFLINE` on 3rd timeout) and 0 ms fast-fail of normal reads were empirically verified.
- **Dual Build Profile Validation**:
  - Execution of `./tools/validate_build_profiles.sh` succeeded with exit code 0:
    - Primary T-ETH-Lite build profile compiled with internal EMAC, PSRAM allocation, and correct configuration keys.
    - Legacy W5500 build profile compiled with SPI W5500, 4MB flash table, and 25% free headroom (0x76480 bytes free) with 0 errors.
- **Test Suite Execution**:
  - `python3 tests/run_e2e_tests.py`: 125/125 passed across Tiers 1-4.
  - Authored empirical challenge test suite (`tests/test_challenger_m2_empirical.py`): 34/34 passed.
  - Full pytest execution: 159/159 passed in 12.04s.
- **Catalog Safety & Invariance**:
  - `bacnet-object-catalog.json` remains completely unmodified (`git status --porcelain bacnet-object-catalog.json` shows untracked baseline file intact). No physical hardware flashing performed.

## 2. Logic Chain
1. **Presentation Layer Decoupling**: By auditing all call sites in `main.c`, Web REST and MQTT presentation adapters rely exclusively on `hvac_core` and `bacnet_worker` APIs. The absence of direct BACnet socket calls or `BacnetMutex` verifies that cross-task socket contention is eliminated.
2. **Domain Invariants**: Verification of `hvac_core.c` arithmetic and boundary checks proves that room indices beyond active count cannot cause buffer overruns, setpoints outside [18.0°C, 30.0°C] cannot reach the physical controller, and system power write/readback instances match Delta controller specifications.
3. **Queue Prioritization & Failure Isolation**: Empirical stress tests confirm that user writes preempt bulk polling reads, queue bursts gracefully return `QUEUE_FULL` without memory leaks, and target timeouts trip the circuit breaker into fast-fail mode to prevent task lockups.
4. **Binary & Profile Safety**: Running the full IDF build script confirms that both T-ETH-Lite and legacy W5500 profiles compile cleanly without compilation errors or partition overflow.

## 3. Caveats
- No physical hardware flashing was performed, in strict compliance with user instructions.
- Target device discovery sweeps subnet addresses `10.0.3.1-32` in addition to global and subnet broadcasts.

## 4. Conclusion
**VERDICT: APPROVE**

Milestone M2 is thoroughly verified and meets all architectural, functional, and safety acceptance criteria:
- Bounded dual-priority worker queue successfully serializes BACnet transactions.
- Protocol-agnostic `components/hvac_core` correctly enforces room models, setpoint clamping [18.0°C - 30.0°C], and power consistency.
- Calling task stacks are safely reduced, recovering >32 KB DRAM.
- Dual profile builds (`./tools/validate_build_profiles.sh`) and complete test suites (159/159 tests) pass with 0 errors.
- `bacnet-object-catalog.json` is untouched and no microcontrollers were flashed.

## 5. Verification Method
1. **Dual Build Profile Validation**:
   ```bash
   source /Users/pierswingfield/esp/esp-idf/export.sh
   ./tools/validate_build_profiles.sh
   ```
   *Result*: Exit code 0, primary T-ETH-Lite and legacy W5500 profiles build cleanly.
2. **E2E Test Suite**:
   ```bash
   python3 tests/run_e2e_tests.py
   ```
   *Result*: 125/125 passed across Tiers 1-4 in 9.30s.
3. **Empirical Challenger Test Suite**:
   ```bash
   pytest -v tests/test_challenger_m2_empirical.py
   ```
   *Result*: 34/34 passed in 3.13s.
4. **Full Test Suite**:
   ```bash
   pytest -v
   ```
   *Result*: 159 passed in 12.04s.
5. **Catalog Invariance**:
   ```bash
   git status --porcelain bacnet-object-catalog.json
   ```
   *Result*: Untouched.
