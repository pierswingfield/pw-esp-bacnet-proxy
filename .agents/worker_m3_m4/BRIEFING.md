# BRIEFING — 2026-09-02T20:38:45Z

## Mission
Implement Milestones M3 & M4: Single Active Integration Mutual Exclusion & T-ETH-Lite Matter Thermostat Endpoint (Cluster 0x0201) PoC.

## 🔒 My Identity
- Archetype: worker
- Roles: implementer, qa, specialist
- Working directory: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/worker_m3_m4
- Original parent: fed7dce7-ef41-4f48-9ccd-5fd974fec9ce
- Milestone: M3 & M4

## 🔒 Key Constraints
- bacnet-object-catalog.json must remain strictly untouched
- DO NOT flash physical firmware
- W5500 legacy profile must build with 0 compile errors and no Matter/BLE bloat
- Mutual exclusion: when Matter or None is active, defer/unallocate all MQTT tasks, queues, and sockets (>28KB DRAM saved)
- All implementations must be genuine - DO NOT cheat or create dummy facades

## Current Parent
- Conversation ID: fed7dce7-ef41-4f48-9ccd-5fd974fec9ce
- Updated: not yet

## Task Summary
- **What to build**:
  1. `firmware/bacnet_bridge/partitions_t_eth_lite.csv`: 16MB flash layout with dual 4MB OTA slots (`ota_0` at 0x30000, `ota_1` at 0x430000), 64K NVS, 24K `matter_fctry` (0x99), and 128K coredump.
  2. Single Active Automation Integration: Mutual exclusion across `HVAC_INTEGRATION_NONE` (0), `HVAC_INTEGRATION_MQTT_HOME_ASSISTANT` (1), and `HVAC_INTEGRATION_MATTER` (2). Persisted in NVS namespace `"nvs_integ"`, key `"mode"`. MQTT resources (>28KB DRAM) dynamically deferred when not in MQTT mode. Web REST APIs `/api/integration` (GET and POST).
  3. Matter Adapter Component (`components/matter_adapter`): CSA Matter Thermostat Cluster (0x0201) standard attribute mapping (Local Temperature 0x0000, Cooling Setpoint 0x0011, Heating Setpoint 0x0012, System Mode 0x001C) routing strictly through canonical `hvac_core`. Compile-time and runtime gating (`CONFIG_ENABLE_ESP_MATTER`).
  4. Dual build profile validation: `./tools/validate_build_profiles.sh` builds both T-ETH-Lite and legacy W5500 with 0 errors.
  5. Test suite verification: Full pass on `pytest` (173/173) and `python3 tests/run_e2e_tests.py` (130/130).
- **Success criteria**: 0 compilation errors across both profiles, 100% E2E tests pass, catalog untouched.

## Key Decisions Made
- Partition layout: `partitions_t_eth_lite.csv` dedicated for 16MB flash profile while preserving 4MB `partitions.csv` for legacy W5500.
- Decoupling presentation layers: Matter adapter translates standard CSA data model attributes to canonical `hvac_core` semantic functions without duplicating room data or touching raw BACnet sockets.
- Dynamic mutual exclusion: When changing integration mode at runtime via `/api/integration`, active transport resources are gracefully torn down and new transport initialized.

## Artifact Index
- `.agents/worker_m3_m4/DISPATCH.md` — Assignment record
- `.agents/worker_m3_m4/BRIEFING.md` — Agent state and situational awareness
- `.agents/worker_m3_m4/progress.md` — Heartbeat and progress tracking
- `.agents/worker_m3_m4/handoff.md` — Comprehensive 5-component handoff report

## Change Tracker
- **Files modified**:
  - `firmware/bacnet_bridge/partitions_t_eth_lite.csv`: Created 16MB partition table.
  - `firmware/bacnet_bridge/sdkconfig.t_eth_lite.defaults`: Set custom partition filename and enabled Matter.
  - `firmware/bacnet_bridge/sdkconfig.w5500.defaults`: Set CONFIG_ENABLE_ESP_MATTER=n.
  - `firmware/bacnet_bridge/components/matter_adapter/include/matter_adapter.h`: Created Matter adapter header.
  - `firmware/bacnet_bridge/components/matter_adapter/matter_adapter.c`: Implemented Matter Thermostat Cluster 0x0201 handlers.
  - `firmware/bacnet_bridge/components/matter_adapter/CMakeLists.txt`: Registered matter_adapter component.
  - `firmware/bacnet_bridge/main/CMakeLists.txt`: Added matter_adapter dependency.
  - `firmware/bacnet_bridge/main/Kconfig.projbuild`: Added CONFIG_ENABLE_ESP_MATTER.
  - `firmware/bacnet_bridge/main/main.c`: Added `/api/integration` endpoints, `mqtt_app_stop`, and runtime mutual exclusion.
  - `tests/tier1_features/test_dual_profile_build.py`: Added T-ETH-Lite partition and Kconfig tests.
  - `tests/tier1_features/test_matter_endpoint.py`: Added heating setpoint, cool/auto, and unsupported mode tests.
  - `PROJECT.md`: Updated M3 & M4 status to DONE.
- **Build status**: PASS (0 errors across T-ETH-Lite and W5500 profiles).
- **Pending issues**: None.

## Quality Status
- **Build/test result**: Dual profile validation PASS; Pytest 173/173 PASS; E2E 130/130 PASS.
- **Lint status**: Clean.
- **Tests added/modified**: +5 tests in tier 1 (total 130 E2E / 173 full pytest suite).

## Loaded Skills
- None
