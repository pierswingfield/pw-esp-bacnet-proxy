# Plan: Protocol-Agnostic HVAC Core & Bounded BACnet Worker Queue + T-ETH-Lite Matter PoC

## Architecture Overview
1. **Protocol-Agnostic HVAC Core**:
   - `components/hvac_core` as Single Source of Truth for canonical room model, active room count, BACnet object mappings, NVS schema, semantic operations (setpoint, power, mode).
   - Transports (Web, MQTT, Matter) interface strictly as presentation/transport adapters with zero direct BACnet calls or duplicate mappings.
2. **Single Active Automation Integration**:
   - `HVAC_INTEGRATION_NONE` (Web only), `HVAC_INTEGRATION_MQTT_HOME_ASSISTANT`, `HVAC_INTEGRATION_MATTER`.
   - Mutual exclusion: MQTT tasks/queues unallocated when Matter is active.
3. **Bounded Single BACnet Worker Queue**:
   - Serialized FreeRTOS queue for all BACnet read/write transactions.
   - Decouples calling tasks, eliminates socket contention and re-entrancy.
   - Priority handling, bounded depth, timeouts, error reporting.
4. **T-ETH-Lite Matter Profile & Partitions**:
   - T-ETH-Lite build profile using ESP-Matter with minimal dependencies.
   - 16MB flash dual enlarged OTA slots (preserving NVS and coredump).
   - Single active-room Matter Thermostat endpoint PoC.
   - W5500 legacy build untouched by Matter.

## Execution Tracks
- **Phase 0: Comprehensive Survey (3 Explorers)**:
  - Explorer 1: Git branch state, build configurations, partition tables, tools/validate_build_profiles.sh.
  - Explorer 2: Existing BACnet stack, client transactions, FreeRTOS tasks, concurrency bottlenecks, worker queue architecture.
  - Explorer 3: Existing HVAC core, web/MQTT integrations, Matter dependencies/integration point, NVS schema.
- **Decomposition into Milestones**:
  - M1: Branch verification & build baseline setup.
  - M2: Bounded Single BACnet Worker Queue implementation & integration with hvac_core.
  - M3: Single Active Automation Integration (Mutual Exclusion) logic & config.
  - M4: T-ETH-Lite Matter profile, partition table update, and single-room Thermostat endpoint.
  - M5: Dual-profile build validation (`./tools/validate_build_profiles.sh`), regression tests, catalog untouched check.
- **E2E Testing Track**:
  - Test runner, mock BACnet server/harness, 4-tier test cases, publishing `TEST_READY.md`.
- **Iteration Loop for each Milestone**:
  - Explorer -> Worker -> Reviewers (2) -> Challengers (2) -> Auditor -> Gate check.
