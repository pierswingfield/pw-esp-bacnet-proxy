# BRIEFING — 2026-09-02T21:06:45+01:00

## Mission
Investigate BACnet communication logic, trace entry points, identify concurrency/re-entrancy bottlenecks, and design the Bounded Single BACnet Worker Queue architecture.

## 🔒 My Identity
- Archetype: explorer
- Roles: BACnet & Worker Queue Explorer
- Working directory: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/explorer_survey_2
- Original parent: fed7dce7-ef41-4f48-9ccd-5fd974fec9ce
- Milestone: Explorer Survey / Worker Queue Design

## 🔒 Key Constraints
- Read-only investigation — do NOT implement or modify project code.
- Write only to `.agents/explorer_survey_2/`.
- Do NOT touch or overwrite `bacnet-object-catalog.json`.
- Do NOT flash physical hardware.
- Complete 5-component handoff report in `handoff.md`.

## Current Parent
- Conversation ID: fed7dce7-ef41-4f48-9ccd-5fd974fec9ce
- Updated: not yet

## Investigation State
- **Explored paths**:
  - `firmware/bacnet_bridge/main/main.c` (all 6952 lines analyzed, including BACnet helpers, cache, web handlers, MQTT tasks, scanner, discovery)
  - `firmware/bacnet_bridge/components/bacnet_client/` (TSM, bip socket, APDU handlers, address binding, build flags)
  - `firmware/bacnet_bridge/components/hvac_core/` (`hvac_core.h`, `hvac_core.c`)
  - `firmware/bacnet_bridge/sdkconfig.defaults`, `sdkconfig.t_eth_lite.defaults`, `partitions.csv`
  - `docs/ARCHITECTURE_AND_API.md`
- **Key findings**:
  - `MAX_TSM_TRANSACTIONS=1` and `Handler_Transmit_Buffer[MAX_PDU]` make the BACnet client stack inherently single-transaction non-reentrant.
  - Current implementation executes `wait_for_transaction()` on caller threads while holding `BacnetMutex`, forcing multiple 24KB stacks across tasks.
  - Multi-threaded callers (15 HTTP endpoints, MQTT commands, 45s periodic telemetry burst of ~50 points, 430-object scan) contend for the single mutex with no request prioritization.
  - Target offline causes a 25-second blocking cascade (50 points x 500ms timeout) that freezes HTTP and MQTT.
- **Unexplored areas**: None within scope; ready for synthesis and handoff specification.

## Key Decisions Made
- Designed a Bounded Dual-Queue Worker Architecture with High Priority (User writes: Web/MQTT/Matter), Normal Priority (Periodic telemetry), and Low Priority (Catalog scan).
- Designed Circuit Breaker / Health Tracker state machine to eliminate offline freeze cascades.
- Designed Synchronous (FreeRTOS semaphore) and Asynchronous (Callback + Cache) completion mechanisms.
- Quantified >40KB internal DRAM savings by consolidating stack requirements into the single worker task.

## Artifact Index
- `.agents/explorer_survey_2/DISPATCH.md` — Record of dispatch prompt
- `.agents/explorer_survey_2/progress.md` — Liveness & heartbeat
- `.agents/explorer_survey_2/BRIEFING.md` — Working memory
- `.agents/explorer_survey_2/handoff.md` — Final 5-component handoff report
