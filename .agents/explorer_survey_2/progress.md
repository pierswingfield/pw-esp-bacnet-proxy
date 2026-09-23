# Progress - Explorer Survey 2 (BACnet & Worker Queue)

Last visited: 2026-09-02T21:06:40+01:00

## Status
- [x] Initialized DISPATCH.md and BRIEFING.md
- [x] Investigating BACnet stack implementation in `firmware/bacnet_bridge/`
  - Explored `components/bacnet_client/` (TSM with MAX_TSM_TRANSACTIONS=1, shared Handler_Transmit_Buffer, bip_socket_esp_idf.c)
  - Analyzed socket creation, packet sending, reception, APDU handlers, address binding
- [x] Tracing all BACnet read/write request origin entry points
  - Cataloged 15 HTTP REST endpoints on `httpd` worker task
  - Traced MQTT command task (`mqtt_command_task`) and periodic polling task (`mqtt_state_task`)
  - Traced object scanner task (`object_scan_task`) and Who-Is discovery (`api_bacnet_discover_handler`)
  - Mapped future Matter Thermostat endpoint interactions
- [x] Identifying concurrency bottlenecks, multi-threaded re-entrancy risks, socket contention
  - Identified single TSM slot limitation vs multi-threaded callers
  - Identified caller-context network polling (`wait_for_transaction` in caller thread) requiring 24KB stacks everywhere
  - Identified lack of socket receive pump when idle and stale packet contamination
  - Identified lack of prioritization (user write starved by 50-point telemetry poll or 430-object scan)
  - Identified target offline timeout cascade (25s blocking per poll cycle)
- [x] Designing the Bounded Single BACnet Worker Queue architecture
  - Defined Request/Response C data structures
  - Defined Dual-Queue priority policy (High priority writes vs Normal telemetry vs Low scan)
  - Defined Bounded queue sizing and queue-full behavior
  - Defined Circuit Breaker / Target Health Tracker state machine (Online/Degraded/Offline)
  - Designed Synchronous (Semaphore/Notification) and Asynchronous (Callback/Cache) completion mechanisms
  - Calculated stack memory savings (>40KB internal DRAM saved)
- [/] Writing comprehensive handoff report (`handoff.md`)
