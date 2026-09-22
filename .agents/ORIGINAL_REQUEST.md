# Original User Request

## 2026-09-02T20:02:57Z

<USER_REQUEST>
Implement the bounded single BACnet worker queue followed by the T-ETH-Lite Matter proof of concept on branch `codex/protocol-agnostic-core` for the ESP32 BACnet bridge, adhering to the protocol-neutral HVAC core architecture and single-active-integration design.

Working directory: `/Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet`
Integrity mode: development

## Architectural Context & Principles

1. **Protocol-Agnostic HVAC Core as Single Source of Truth:**
   - `components/hvac_core` owns the canonical room model, active room count, room-to-BACnet mapping, NVS schema, and semantic operations (setpoint, power, temperature).
   - External integrations (Web UI, MQTT, Matter) are strictly presentation/transport adapters. They must NEVER duplicate room configurations, store independent mappings, or execute direct BACnet calls.
2. **Single Active Automation Integration (Mutual Exclusion):**
   - At runtime, exactly one automation transport is active:
     - `HVAC_INTEGRATION_NONE`: Web dashboard only.
     - `HVAC_INTEGRATION_MQTT_HOME_ASSISTANT`: Existing MQTT client and Home Assistant discovery.
     - `HVAC_INTEGRATION_MATTER`: Matter-over-Wi-Fi (T-ETH-Lite profile only).
   - Matter and MQTT are mutually exclusive. When Matter is active, MQTT tasks, queues, and network buffers must remain unallocated/deferred.
   - The Web dashboard, BACnet worker, local OTA, and diagnostic tools remain active across all modes.
3. **Hardware Boundary:**
   - **T-ETH-Lite** (ESP32-WROVER-E, 16MB flash, 8MB PSRAM) is the sole target for Matter.
   - **W5500** (ESP32-WROOM, 4MB flash, no PSRAM) is legacy/recovery only, restricted to MQTT/web, and must remain completely untouched by Matter dependencies.

## Requirements

### R1. Bounded Single BACnet Worker Queue
- Route all BACnet read and write transactions (web dashboard, MQTT commands/polling, and future Matter callbacks) through a dedicated, serialized, bounded BACnet worker queue.
- Decouple calling tasks from synchronous BACnet network transactions, eliminating cross-task socket contention and multi-threaded BACnet client stack re-entrancy.
- Provide bounded queue depth, request prioritization (e.g. user write commands over periodic telemetry polling), and clean error/timeout handling when the BACnet target is unreachable.
- Preserve existing MQTT topics, Home Assistant entity IDs/discovery, and web dashboard response contracts without behavioral regression.

### R2. T-ETH-Lite Matter Proof of Concept
- Add a T-ETH-Lite Matter build profile utilizing Espressif ESP-Matter with minimal external dependencies.
- Update partition table configurations to provide enlarged dual OTA slots required for Matter images (on the 16MB flash profile) while preserving NVS and coredump partitions.
- Implement a single active-room Matter Thermostat endpoint proof of concept:
  - Expose standard thermostat attributes (Local Temperature, Occupied Cooling/Heating Setpoint, System Mode).
  - Route state changes and setpoint adjustments through `hvac_core` and the BACnet worker queue.
  - Keep Delta-specific vendor features (boost mode, diagnostics, object browser) strictly in the web dashboard rather than creating non-standard Matter clusters.
- Enforce the mutual exclusion rule: ensure Matter stack initialization is gated by integration selection, deferring all MQTT tasks and allocations.

### R3. Profile, Safety, and Environment Constraints
- Maintain the legacy W5500 profile buildability (`sdkconfig.w5500.defaults`) with 0 compile errors and no Matter/BLE bloat.
- Keep `bacnet-object-catalog.json` strictly untouched (commissioning reference file).
- Do not flash physical firmware to connected microcontrollers during this session.

## Verification Resources & Acceptance Criteria

### Verification Commands
- ESP-IDF environment: source `/Users/pierswingfield/esp/esp-idf/export.sh` (ESP-IDF v5.3.1).
- T-ETH-Lite build command:
  ```bash
  cd firmware/bacnet_bridge && idf.py -B build-t-eth -DSDKCONFIG=build-t-eth/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.t_eth_lite.defaults" build
  ```
- Dual-profile regression validation:
  ```bash
  ./tools/validate_build_profiles.sh
  ```

### Acceptance Criteria
- [ ] BACnet read and write transactions from web and MQTT transports execute through the bounded worker queue; no raw concurrent BACnet socket writes bypass the queue.
- [ ] `hvac_core` remains the single authority for room configuration and semantic commands; no transport duplicates room mapping.
- [ ] Runtime integration selection cleanly isolates MQTT vs Matter: when Matter is selected, MQTT task stacks and client memory are not allocated.
- [ ] T-ETH-Lite profile compiles cleanly with Matter enabled and fits within the updated partition table boundaries with healthy free headroom.
- [ ] `./tools/validate_build_profiles.sh` passes, verifying that both T-ETH-Lite and legacy W5500 builds succeed without regressions.
- [ ] `git status` verifies `bacnet-object-catalog.json` remains completely unmodified.
- [ ] No physical hardware flashing was executed.

</USER_REQUEST>
