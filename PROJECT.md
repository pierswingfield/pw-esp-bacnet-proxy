# Project: Protocol-Agnostic HVAC Core, Bounded BACnet Worker Queue & T-ETH-Lite Matter PoC

## Architecture
- **Protocol-Agnostic HVAC Core (`components/hvac_core`)**:
  - Acts as the Single Source of Truth for canonical room models, active room count, BACnet object instance mappings, NVS schema (`nvs_rooms`, `nvs_integ`), and semantic operations (room setpoint, room power, system power, boost mode).
  - External integrations (Web Dashboard, MQTT client, Matter endpoint) are strictly presentation/transport adapters with zero duplicate room mappings or direct BACnet network socket calls.
- **Bounded Single BACnet Worker Queue (`components/bacnet_client/bacnet_worker.*` or `components/hvac_core`)**:
  - Serializes all BACnet confirmed request transactions (read/write property, who-is discovery, probe liveness).
  - Dual-queue priority mechanism: High-priority queue (depth 8) for user-initiated write commands; Normal-priority queue (depth 24) for periodic telemetry polling and background reads.
  - Circuit Breaker / Health Tracker state machine (`ONLINE`, `DEGRADED`, `OFFLINE`) to fast-fail dead-target telemetry and prevent freeze cascades.
  - Dedicated worker task (`bacnet_worker_task`) owns the UDP socket and APDU decoding, shrinking calling task stacks (`mqtt_command_task`, `mqtt_state_task`, `httpd`) from 16–24KB down to 4KB and recovering >40KB internal DRAM.
- **Single Active Automation Integration (Mutual Exclusion)**:
  - Gated via `hvac_core_integration_get()`:
    - `HVAC_INTEGRATION_NONE` (0): Web dashboard only.
    - `HVAC_INTEGRATION_MQTT_HOME_ASSISTANT` (1): MQTT client & HA discovery active.
    - `HVAC_INTEGRATION_MATTER` (2): Matter Thermostat endpoint active (T-ETH-Lite only).
  - When Matter or None is active, MQTT task stacks (20.5KB), command queue (1.15KB), and network buffers/TLS context remain unallocated.
- **Dual Build Profiles & Partition Isolation**:
  - **T-ETH-Lite Profile**: ESP32-WROVER-E (16MB Flash, 8MB PSRAM), internal EMAC + RTL8201 PHY, dual 4096KB OTA slots in `partitions_t_eth_lite.csv`, Matter Thermostat endpoint enabled.
  - **W5500 Profile**: ESP32-WROOM (4MB Flash, No PSRAM), SPI W5500 Ethernet, dual 1900KB OTA slots in `partitions.csv`, Matter disabled (`CONFIG_ENABLE_ESP_MATTER=n`), 0 compile errors and zero Matter/BLE bloat.
  - Commissioning reference `bacnet-object-catalog.json` remains strictly untouched. No physical hardware flashing.

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Git Branch & Baseline Setup | Verify branch `codex/protocol-agnostic-core`, check baseline build | M1 | Survey |
| 2 | Canonical Room Model | `hvac_room_config_t`, `HvacRooms`, `HvacRoomCount` in `components/hvac_core` | M2 | Survey |
| 3 | Room NVS Persistence | Store & load room names, active states, and object instance IDs in `"nvs_rooms"` | M2 | Survey |
| 4 | Bounded BACnet Worker Queue | Dual-priority FreeRTOS queue (High: 8, Normal: 24) serializing BACnet calls | M2 | Survey |
| 5 | Worker Network Polling Pump | Dedicated `bacnet_worker_task` owning socket, TSM, and APDU decoding | M2 | Survey |
| 6 | Circuit Breaker & Target Health | Health tracker (`ONLINE`/`DEGRADED`/`OFFLINE`) with 5s recovery probe | M2 | Survey |
| 7 | Synchronous & Async Caller APIs | `bacnet_worker_read_*`, `bacnet_worker_write_*`, explorer sync read/write | M2 | Survey |
| 8 | Web REST Integration to Worker | Route `/api/status`, `/api/health`, `/api/bacnet/*`, `/api/room/*` via worker | M2 | Survey |
| 9 | MQTT Integration to Worker | Route `mqtt_command_task` and `mqtt_state_task` via worker queue | M2 | Survey |
| 10 | Task Stack Memory Optimization | Shrink `mqtt_command_task` (24K->4K), `mqtt_state_task` (16K->4K), saving DRAM | M2 | Survey |
| 11 | Integration Mode NVS Schema | Store & load integration mode (`nvs_integ:mode`, default=MQTT) in `hvac_core` | M3 | Survey |
| 12 | Mutual Exclusion Gating | Cleanly defer MQTT task stacks, queues, and network buffers when not MQTT | M3 | Survey |
| 13 | Web UI Integration Mode Switch | Web endpoint / UI to configure integration mode with runtime gating | M3 | Survey |
| 14 | T-ETH-Lite 16MB Partition Table | `partitions_t_eth_lite.csv` with dual 4MB OTA slots, NVS, coredump | M4 | Survey |
| 15 | T-ETH-Lite Matter Kconfig & Build | `sdkconfig.t_eth_lite.defaults` configured for custom partition and Matter | M4 | Survey |
| 16 | Matter Thermostat Endpoint (Cluster 0x0201) | Expose primary active room (Room A) as standard Thermostat endpoint | M4 | Survey |
| 17 | Matter LocalTemperature Read | Map AV:1101/AV:1201 to Thermostat attribute 0x0000 (0.01°C) | M4 | Survey |
| 18 | Matter Occupied Setpoint Write/Read | Map AV:1100/AV:1200 to Thermostat attributes 0x0011/0x0012 via `hvac_core` | M4 | Survey |
| 19 | Matter SystemMode Control | Map BV:1101/BV:1201 to Thermostat attribute 0x001C via `hvac_core` | M4 | Survey |
| 20 | Vendor Feature Web Confinement | Keep boost mode, catalog browser, diagnostics strictly in Web UI | M4 | Survey |
| 21 | Dual Profile Build Validation | Ensure `./tools/validate_build_profiles.sh` passes with 0 errors | M5 | Survey |
| 22 | Catalog Invariance & Non-Flashing | Ensure `bacnet-object-catalog.json` untouched and no physical flash | M5 | Survey |
| 23 | E2E Test Suite Pass (Tiers 1-4) | Pass 100% of requirement-driven E2E test cases | M5 | Survey |
| 24 | Adversarial Coverage Hardening (Tier 5) | White-box stress and corner case validation | M5 | Survey |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Branch & Git Baseline Setup | Verify branch `codex/protocol-agnostic-core`, working tree baseline, catalog invariance | none | DONE |
| M2 | Bounded Single BACnet Worker Queue & HVAC Core | Implement `bacnet_worker.*`, decouple callers (Web/MQTT), integrate `components/hvac_core`, reduce task stacks | M1 | DONE |
| M3 | Single Active Integration & Mutual Exclusion | Enforce `HVAC_INTEGRATION_*` mutual exclusion, defer MQTT resources when not selected, verify NVS persistence | M2 | DONE |
| M4 | T-ETH-Lite Matter Profile, Partitions & Thermostat PoC | Add `partitions_t_eth_lite.csv`, Matter build profile, Matter Thermostat Endpoint PoC for Room A | M3 | DONE |
| M5 | Dual-Profile Build & E2E Validation | Pass `./tools/validate_build_profiles.sh`, execute 100% E2E tests, Tier 5 adversarial hardening, Forensic Audit | M4 | DONE |
| M6 | Real ESP-Matter SDK Integration & Live Commissioning | Link real `esp-matter`/CHIP (not stub scaffolding), fix DRAM/heap/lock bugs blocking boot, verify live mDNS advertising + commissioning window on hardware | M5 | DONE — see `docs/MATTER_INTEGRATION_SESSION_2026-09-12.md` |
| M7 | Automation Module Selector & Pairing-Status UI | Replace hardcoded MQTT-only wizard step with a real mode selector (Dashboard/MQTT/Matter), persist Matter pairing status, add retry-pairing UI | M6 | IN PROGRESS — backend + wizard/health HTML written and building; **not yet visually verified in a browser** due to an open HTTP-reset bug, see session doc |
| M8 | HTTP-reset bug root cause | A second, distinct dashboard-unresponsive failure (`Connection reset by peer`, not the known BLE-coexistence timeout) appeared late in the M7 session and is unresolved | M7 | OPEN — see `docs/MATTER_INTEGRATION_SESSION_2026-09-12.md` §"Open issue" |

## Interface Contracts
### `hvac_core` ↔ Presentation Layers (Web, MQTT, Matter)
- `hvac_room_config_t *hvac_core_get_room(size_t room_idx)`: Read canonical room config.
- `size_t hvac_core_get_room_count(void)`: Get active room count (max 8).
- `bool hvac_core_set_room_setpoint(size_t room_idx, float requested, float *applied)`: Semantic setpoint update.
- `bool hvac_core_set_room_power(size_t room_idx, bool on)`: Semantic room power update.
- `bool hvac_core_set_system_power(bool on)`: Semantic master system power update.
- `hvac_integration_kind_t hvac_core_integration_get(void)`: Get active automation integration.
- `esp_err_t hvac_core_integration_set(hvac_integration_kind_t mode)`: Persist & update active integration.

### `bacnet_worker` ↔ `hvac_core` & Transports
- `bool bacnet_worker_read_real(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, float *out_val)`
- `bool bacnet_worker_read_bool(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, bool *out_val)`
- `bool bacnet_worker_read_msv(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, unsigned *out_val)`
- `bool bacnet_worker_write_real(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, float val)`
- `bool bacnet_worker_write_bool(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, bool val)`
- `bool bacnet_worker_write_msv(BACNET_OBJECT_TYPE type, uint32_t instance, BACNET_PROPERTY_ID prop, unsigned val)`
- `bacnet_status_t bacnet_worker_explorer_read_sync(...)`
- `bacnet_status_t bacnet_worker_explorer_write_sync(...)`

## Code Layout
- `firmware/bacnet_bridge/components/hvac_core/`: Protocol-agnostic HVAC domain model, NVS schemas, room definitions.
- `firmware/bacnet_bridge/components/bacnet_client/`: BACnet/IP client stack, `bacnet_worker.h`, `bacnet_worker.c`.
- `firmware/bacnet_bridge/components/matter_adapter/` (or conditional main matter source): Matter Thermostat endpoint handler and cluster callbacks.
- `firmware/bacnet_bridge/main/`: Web server handlers, MQTT client task, initialization, system glue.
- `firmware/bacnet_bridge/partitions.csv`: 4MB partition table for W5500 legacy profile.
- `firmware/bacnet_bridge/partitions_t_eth_lite.csv`: 16MB partition table for T-ETH-Lite Matter profile.
- `tools/validate_build_profiles.sh`: Dual-profile compilation validator.
