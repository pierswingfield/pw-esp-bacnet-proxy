## 2026-09-02T20:04:01Z

You are the HVAC Core & Matter Specification Miner for the ESP-BACnet project.
Working directory: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/spec_miner_survey_3
Original User Request: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/ORIGINAL_REQUEST.md

Task:
1. Investigate the current HVAC room model, room-to-BACnet object mappings, and NVS storage schema across the project (inspect `components/hvac_core` if present, `firmware/bacnet_bridge/main/`, web dashboard, and MQTT integration).
2. Determine how `components/hvac_core` must be structured as the Single Source of Truth for canonical room models, active room count, semantic operations (setpoint, power, temperature), and room-to-BACnet mappings.
3. Investigate the Single Active Automation Integration mechanism:
   - `HVAC_INTEGRATION_NONE` (Web dashboard only)
   - `HVAC_INTEGRATION_MQTT_HOME_ASSISTANT` (Existing MQTT client & HA discovery)
   - `HVAC_INTEGRATION_MATTER` (Matter Thermostat endpoint)
   - Define exact mutual exclusion rules: how MQTT tasks, queues, and buffers are deferred/unallocated when Matter is selected.
4. Investigate Matter Thermostat endpoint requirements on ESP-Matter / ESP-IDF for T-ETH-Lite:
   - Standard thermostat cluster attributes (Local Temperature, Occupied Cooling/Heating Setpoint, System Mode).
   - Mapping between Matter cluster attribute reads/writes and `hvac_core` / BACnet worker queue.
   - How vendor-specific features (boost mode, diagnostics, object browser) stay exclusively in web dashboard.
   - ESP-Matter dependency management (ensuring W5500 legacy build has 0 Matter dependencies).
5. Document all findings, data structures, API contracts, and integration state machines in `/Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/spec_miner_survey_3/handoff.md` and keep `progress.md` updated.
