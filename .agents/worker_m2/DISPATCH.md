# DISPATCH

## 2026-09-02T20:08:55Z

You are the Implementation Worker for Milestone M2: Bounded Single BACnet Worker Queue & HVAC Core Integration.
Working directory: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/worker_m2
Original User Request: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/ORIGINAL_REQUEST.md
Project Specification: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/PROJECT.md
Explorer Survey 2 Report: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/explorer_survey_2/handoff.md
Spec Miner Survey 3 Report: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/spec_miner_survey_3/handoff.md

Please read /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/ORIGINAL_REQUEST.md first.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

File Write Ownership:
- firmware/bacnet_bridge/components/bacnet_client/bacnet_worker.h
- firmware/bacnet_bridge/components/bacnet_client/bacnet_worker.c
- firmware/bacnet_bridge/components/bacnet_client/CMakeLists.txt
- firmware/bacnet_bridge/components/hvac_core/
- firmware/bacnet_bridge/main/main.c

Task:
1. Implement the Bounded Single BACnet Worker Queue (bacnet_worker.h and bacnet_worker.c in components/bacnet_client) adhering to the architecture specified in explorer_survey_2/handoff.md:
   - Dual-priority FreeRTOS queues: High-priority (depth 8) for user writes, Normal-priority (depth 24) for telemetry reads.
   - Dedicated bacnet_worker_task owning the UDP socket, TSM (MAX_TSM_TRANSACTIONS=1), and APDU decoding.
   - Target Health Circuit Breaker (ONLINE, DEGRADED, OFFLINE) with fast-fail when offline and 5s periodic probe.
   - Synchronous and asynchronous completion helpers (bacnet_worker_read_real, bacnet_worker_write_real, bacnet_worker_read_bool, bacnet_worker_write_bool, bacnet_worker_read_msv, bacnet_worker_write_msv, explorer read/write sync).
2. Refactor components/hvac_core and firmware/bacnet_bridge/main/main.c:
   - Route all BACnet read/write calls (Web REST handlers, MQTT command task, MQTT state telemetry task) through the BACnet worker queue. Ensure NO raw Send_Read_Property_Request or Send_Write_Property_Request remain un-serialized in caller threads.
   - Ensure components/hvac_core is the Single Source of Truth for room models, NVS persistence, and semantic operations.
   - Reduce task stack sizes in main.c: shrink mqtt_command_task stack from 24K to 4096, mqtt_state_task stack from 16K to 4096 to recover internal DRAM.
3. Verify compilation:
   - Source /Users/pierswingfield/esp/esp-idf/export.sh
   - Run ./tools/validate_build_profiles.sh and ensure both T-ETH-Lite and W5500 compile with 0 errors.
4. Constraints:
   - bacnet-object-catalog.json must remain strictly untouched.
   - DO NOT flash physical firmware.
5. Write handoff.md in your working directory with observation, logic chain, caveats, conclusion, and verification command output. Keep progress.md updated.

## 2026-09-02T20:14:45Z

**Context**: Compilation validation for Milestone M2 (Bounded BACnet Worker Queue).
**Content**: Test writer detected two compilation issues in your new code:
1. Macro collision: `bacnet/bacdef.h` defines `#define BACNET_STATUS_OK (0)` which conflicts with enum member `BACNET_STATUS_OK` in `bacnet_worker.h`. Please prefix worker status enum values (e.g. `BACNET_WORKER_STATUS_OK`, `BACNET_WORKER_STATUS_TIMEOUT`, etc.).
2. Header path: `bacnet_worker.c` included `"bacnet/datalink/bip_init.h"`, whereas the valid header in `components/bacnet_client/bacnet/datalink/` is `"bacnet/datalink/bip.h"`.
**Action**: Ensure `bacnet_worker.h` and `bacnet_worker.c` use prefixed enum names and valid headers, and verify that `./tools/validate_build_profiles.sh` completes with exit code 0.
