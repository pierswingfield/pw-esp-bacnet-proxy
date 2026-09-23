## 2026-09-02T20:04:01Z

You are the BACnet & Worker Queue Explorer for the ESP-BACnet project.
Working directory: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/explorer_survey_2
Original User Request: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/ORIGINAL_REQUEST.md

Please read /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/ORIGINAL_REQUEST.md first.

Task:
1. Thoroughly investigate all BACnet communication logic in `firmware/bacnet_bridge/` (BACnet client stack, read/write property functions, who-is/i-am, socket bindings, tasks, polling routines).
2. Trace all entry points where BACnet read/write requests currently originate (web handlers, REST API, MQTT command subscriptions, background polling tasks, etc.).
3. Identify all concurrency bottlenecks, multi-threaded re-entrancy risks, socket contention, and lack of serialization.
4. Design the Bounded Single BACnet Worker Queue architecture:
   - Request structure (operation type, target device/object ID/property, write value, callback / response channel, priority, timeout).
   - Priority policy (high priority for user-initiated write commands from Web/MQTT/Matter; low/normal priority for periodic telemetry polling).
   - Bounded queue sizing and queue-full behavior.
   - Error handling, timeouts, retry policy, and target offline/unreachable handling.
   - Decoupling callers: synchronous vs asynchronous completion mechanisms (e.g. FreeRTOS task notifications, semaphore/event group, or completion callbacks).
5. Document all findings, current code call graphs, and exact worker queue design specifications in `/Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/explorer_survey_2/handoff.md` and keep `progress.md` updated.
