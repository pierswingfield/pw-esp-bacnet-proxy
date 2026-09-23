# Handoff Report — Reviewer 1: Milestone M2

## 1. Observation

### Implementation & Architecture
- **Single BACnet Worker Task & Static Memory**:
  `firmware/bacnet_bridge/components/bacnet_client/bacnet_worker.c` allocates static task memory (`s_worker_tcb`, `s_worker_stack` with `BACNET_WORKER_STACK_SIZE = 24576`) and creates `bacnet_worker_task` (priority 5).
  `bacnet_worker_task` exclusively manages `bip_init`, `bip_receive`, `npdu_handler`, `tsm_timer_milliseconds`, and all confirmed/unconfirmed APDU dispatches (`Send_Read_Property_Request`, `Send_Write_Property_Request`, `Send_WhoIs_*`).
- **Bounded Dual-Priority Queues**:
  `bacnet_worker.c` (lines 38–43, 631–638) configures static FreeRTOS queues: `s_high_prio_queue` (depth 8) for user write commands and discovery, and `s_normal_prio_queue` (depth 24) for background telemetry reads. In the worker loop (lines 562–567), `s_high_prio_queue` is drained with 0 ticks timeout before `s_normal_prio_queue`, ensuring preemption of writes over reads.
- **Circuit Breaker & Target Health**:
  `bacnet_worker.c` (lines 55–58, 414–421, 574–576, 606–611) maintains `s_health_state` (`ONLINE`, `DEGRADED`, `OFFLINE`). When 3 consecutive timeouts occur, state transitions to `OFFLINE`. When `OFFLINE`, normal-priority read requests fast-fail immediately (`BACNET_WORKER_STATUS_TARGET_OFFLINE`, 0 ms delay) without touching the network. An automatic 5000 ms periodic probe (`ReadProperty(OBJECT_DEVICE, s_target_dev_id, PROP_OBJECT_NAME)`) monitors target recovery and restores state to `ONLINE` upon receiving a reply.
- **Thread-Safe Read Cache**:
  `bacnet_worker.c` (lines 76–187) implements a 64-entry cache with a 30-second TTL protected by `s_cache_mux` spinlocks (`taskENTER_CRITICAL` / `taskEXIT_CRITICAL`). Successful writes explicitly invalidate matching cache entries (`bacnet_cache_invalidate`).
- **Single Source of Truth (`components/hvac_core`)**:
  `firmware/bacnet_bridge/components/hvac_core/hvac_core.h` and `hvac_core.c` own the canonical `HvacRooms` array (capacity 8), active room count, and NVS serialization (`nvs_rooms` namespace). All semantic operations (`hvac_core_set_room_setpoint`, `hvac_core_set_room_power`, `hvac_core_set_system_power`, `hvac_core_get_room_*`) route strictly through `bacnet_worker_*`.
- **Zero Raw BACnet Call Leaks**:
  Grep search across `firmware/bacnet_bridge/main/` confirmed 0 occurrences of direct `Send_*`, `bip_*`, `npdu_*`, or `apdu_*` calls. All Web endpoints (`/api/room-setpoint`, `/api/room-power`, `/api/system-power`, `/api/bacnet/read`, `/api/bacnet/write`, `/api/bacnet/discover`), MQTT handlers (`mqtt_command_task`, `mqtt_state_task`), and background scans (`object_scan_task`) route strictly through `hvac_core_*` and `bacnet_worker_*`.
- **Task Stack Reductions & Mutual Exclusion**:
  `main.c` reduced `mqtt_command_task` stack from 24,576 bytes to 4,096 bytes and `mqtt_state_task` stack from 16,384 bytes to 4,096 bytes, reclaiming >32 KB internal DRAM. Mutual exclusion is enforced via `mqtt_integration_selected()`: when Matter is active, MQTT tasks and queues remain unallocated.

### Integrity & Security Audit
- No hardcoded test outputs or dummy return facades were found in source code.
- No shortcuts or bypassed logic detected.
- `bacnet-object-catalog.json` is completely unmodified (SHA-256: `5d4365466ee5768f3f0c92d0801aa559e0aa2f7107c66274d77f65cca2d171b8`).
- Zero physical hardware flashing commands were executed.

### Build and Test Execution
- `./tools/validate_build_profiles.sh`:
  - Primary T-ETH-Lite profile: Build succeeded (binary size: 0x164b80 bytes, 25% free in app partition).
  - Legacy W5500 profile: Build succeeded (0 compilation errors, 0 link errors).
- `python3 tests/run_e2e_tests.py`:
  - Tier 1 (Feature Coverage): 51 / 51 passed.
  - Tier 2 (Boundary & Corner Cases): 51 / 51 passed.
  - Tier 3 (Cross-Feature Combinations): 18 / 18 passed.
  - Tier 4 (Real-World Scenarios): 5 / 5 passed.
  - Total: 125 / 125 passed (100% pass rate).
- `pytest tests/test_challenger_m2_empirical.py`:
  - 34 / 34 passed (100% pass rate).

---

## 2. Logic Chain

1. **Serialization & Concurrency Safety**: Because all BACnet transactions across the HTTP web server, MQTT command/state loops, and object scan tasks now dispatch into FreeRTOS queues consumed exclusively by `bacnet_worker_task`, multi-threaded re-entrancy into `bacnet-stack` and global buffer stomping (`Handler_Transmit_Buffer`, `MAX_TSM_TRANSACTIONS=1`) are structurally eliminated.
2. **Prioritization & Starvation Prevention**: Because `bacnet_worker_task` prioritizes `s_high_prio_queue` over `s_normal_prio_queue`, interactive write commands and discoveries execute ahead of background telemetry bursts.
3. **Resilience & Fault Confinement**: When the BACnet controller is unreachable, the 3-timeout circuit breaker isolates the rest of the firmware from blocking, ensuring HTTP and MQTT tasks remain fully responsive while background probing automatically restores online status upon recovery.
4. **Domain Centralization**: Making `components/hvac_core` the single authority ensures that presentation adapters (Web, MQTT, Matter) cannot drift in room mapping, NVS schema, or semantic control rules.
5. **Memory Efficiency**: Removing APDU encode/decode buffers from caller tasks enabled safe reduction of caller stacks from 24 KB / 16 KB down to 4 KB each, freeing >32 KB DRAM on constrained hardware.

---

## 3. Caveats

- **Extreme Queue Congestion Advisory**: In `bacnet_worker_dispatch_sync`, caller threads wait on a per-request binary semaphore with timeout `(req->timeout_ms + 200)`. If the normal queue is congested with maximum entries (24) under an offline-transitioning target before tripping the circuit breaker, caller timeouts may expire before dequeue. The circuit breaker's fast-fail mechanism mitigates this under sustained offline conditions.
- **Physical Hardware**: No physical flashing was conducted, per explicit instruction. Hardware validation will proceed during scheduled bench testing.

---

## 4. Conclusion

**Verdict: APPROVE**

Milestone M2 ("Bounded Single BACnet Worker Queue & HVAC Core Integration") satisfies all architectural, performance, safety, and integrity requirements:
- Single worker queue serialization is completely implemented.
- `hvac_core` is the verified Single Source of Truth for room models.
- Dual-profile firmware builds cleanly for both T-ETH-Lite and legacy W5500 targets.
- 100% of E2E tests (125/125) and challenger tests (34/34) pass.
- Catalog invariance and non-flashing constraints are strictly respected.

---

## 5. Verification Method

To independently reproduce and verify this assessment:

1. **Dual Build Profile Validation**:
   ```bash
   source /Users/pierswingfield/esp/esp-idf/export.sh
   ./tools/validate_build_profiles.sh
   ```
   *Expected Output*: Exit code 0, message `Profile validation passed: primary T-ETH-Lite and legacy W5500.`

2. **Full E2E Test Suite Execution**:
   ```bash
   python3 tests/run_e2e_tests.py
   ```
   *Expected Output*: `125 passed, 0 failed, 0 errors in ~9.5s`.

3. **Empirical Challenger Suite Execution**:
   ```bash
   pytest tests/test_challenger_m2_empirical.py -v
   ```
   *Expected Output*: `34 passed in ~2.3s`.

4. **Catalog Integrity Audit**:
   ```bash
   shasum -a 256 bacnet-object-catalog.json
   ```
   *Expected Output*: `5d4365466ee5768f3f0c92d0801aa559e0aa2f7107c66274d77f65cca2d171b8`.
