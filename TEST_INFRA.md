# E2E Test Infra: ESP-BACnet Bridge

## Test Philosophy
- Opaque-box, requirement-driven test verification derived directly from `ORIGINAL_REQUEST.md`.
- Zero coupling to internal implementation quirks.
- Category-Partition + Boundary Value Analysis (BVA) + Pairwise Combinatorial Testing + Real-World Workload Testing.
- Progressive testability: verification methods do not depend on features under test.

## Feature Inventory & Test Mapping
| # | Feature Area | Requirement | Tier 1 (Coverage) | Tier 2 (Boundaries) | Tier 3 (Cross-Feature) | Tier 4 (Real-World) |
|---|--------------|-------------|:-----------------:|:-------------------:|:----------------------:|:-------------------:|
| 1 | Canonical HVAC Core | Room configuration, active room count, semantic operations | 5 tests | 5 tests | Pairwise | Scenario 1, 2 |
| 2 | Room NVS Persistence | Store/load `nvs_rooms`, `nvs_integ` | 5 tests | 5 tests | Pairwise | Scenario 3 |
| 3 | Bounded BACnet Queue | Serialization, dual-priority, queue limits | 5 tests | 5 tests | Pairwise | Scenario 4 |
| 4 | Worker APDU Pump & Concurrency | Single-socket owner, thread safety under multi-task load | 5 tests | 5 tests | Pairwise | Scenario 4 |
| 5 | Target Health & Circuit Breaker | Fast-fail on offline target, periodic recovery probe | 5 tests | 5 tests | Pairwise | Scenario 5 |
| 6 | Mutual Exclusion & Deferral | None vs MQTT vs Matter resource isolation | 5 tests | 5 tests | Pairwise | Scenario 2, 3 |
| 7 | T-ETH-Lite Matter Endpoint | Cluster 0x0201 Thermostat attributes (Temp, Setpoint, Mode) | 5 tests | 5 tests | Pairwise | Scenario 1 |
| 8 | Vendor Feature Confinement | Boost, catalog browser, diagnostics confined to Web | 5 tests | 5 tests | Pairwise | Scenario 1, 5 |
| 9 | Dual Profile Compilation | T-ETH-Lite (16MB) vs W5500 (4MB) with 0 errors | 5 tests | 5 tests | Pairwise | Scenario 5 |
| 10 | Catalog & Safety Invariance | `bacnet-object-catalog.json` untouched, no flashing | 5 tests | 5 tests | Pairwise | All Scenarios |

## Test Architecture
- **Location**: `tests/`
- **E2E Test Runner**: `tests/run_e2e_tests.py` / `tests/test_hvac_suite.py`
- **Mock BACnet Server / Datalink Simulator**: Simulates Delta DAC-1180E response behavior, APDU delays, packet loss, target offline conditions.
- **Pass/Fail Semantics**: All test suites must return exit code 0.
- **Dual Profile Validation**: `./tools/validate_build_profiles.sh`

## Real-World Application Scenarios (Tier 4)
| # | Scenario | Features Exercised | Complexity |
|---|----------|--------------------|------------|
| 1 | Multi-Transport Room Control Flow | Web dashboard setpoint change concurrent with Matter thermostat mode adjustment and background telemetry. | High |
| 2 | Automation Mode Transition Cycle | Runtime switch from MQTT to Matter to None and back to MQTT, verifying dynamic memory allocation/deferral. | High |
| 3 | NVS Cold Boot Persistence & Corruption Resilience | Power-cycle simulation with valid, missing, and partial NVS configurations. | Medium |
| 4 | High-Concurrency BACnet Burst & Target Recovery | 50-point telemetry polling interrupted by simultaneous Web/MQTT user setpoint writes, followed by simulated link down/up. | High |
| 5 | Dual Profile Build & Binary Constraint Audit | End-to-end clean compilation of T-ETH-Lite (with Matter) and W5500 (without Matter), verifying partition fits and catalog integrity. | Medium |

## Coverage Thresholds
- **Tier 1 (Feature Coverage)**: ≥ 50 test cases across all 10 feature areas.
- **Tier 2 (Boundary & Corner Cases)**: ≥ 50 test cases across all limit conditions.
- **Tier 3 (Cross-Feature Combinations)**: ≥ 15 pairwise interaction tests.
- **Tier 4 (Real-World Application Scenarios)**: 5 comprehensive application scenario suites.
- **Tier 5 (Adversarial Coverage Hardening)**: White-box challenger generated edge tests.
