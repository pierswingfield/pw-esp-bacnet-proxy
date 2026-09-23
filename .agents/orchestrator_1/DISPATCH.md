# Dispatch Log

## 2026-09-02T20:03:19Z

You are the Project Orchestrator for this task.
Your working directory is: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/orchestrator_1/
Workspace root: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet
Original User Request is recorded in: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/ORIGINAL_REQUEST.md

Please review the full request in /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/ORIGINAL_REQUEST.md.

Key Goals & Requirements:
1. Ensure we are on branch `codex/protocol-agnostic-core` (or switch/create as appropriate based on existing git state).
2. Protocol-Agnostic HVAC Core as Single Source of Truth (`components/hvac_core`).
3. Single Active Automation Integration (Mutual Exclusion): `HVAC_INTEGRATION_NONE`, `HVAC_INTEGRATION_MQTT_HOME_ASSISTANT`, `HVAC_INTEGRATION_MATTER`.
4. R1: Bounded Single BACnet Worker Queue (route all BACnet read/write through worker queue, serialization, bounded depth, priority, timeout handling, no regressions on web/MQTT).
5. R2: T-ETH-Lite Matter Proof of Concept (T-ETH-Lite Matter build profile, enlarged dual OTA partitions on 16MB flash, single active-room Matter Thermostat endpoint PoC, integration gating).
6. R3: Profile, Safety, Environment (legacy W5500 profile buildability with 0 errors and no Matter bloat, keep `bacnet-object-catalog.json` strictly untouched, DO NOT flash physical firmware).
7. Dual-profile regression validation: `./tools/validate_build_profiles.sh` must pass.

Maintain your `plan.md`, `progress.md`, and `BRIEFING.md` inside your working directory.
When finished, send a completion message with detailed verification evidence and report to the sentinel.
