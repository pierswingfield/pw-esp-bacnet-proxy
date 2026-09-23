# BRIEFING — 2026-09-02T20:40:00Z

## Mission
Adversarial empirical stress-testing and verification for Milestone M5: Single active automation integration mutual exclusion (Matter vs MQTT), T-ETH-Lite Matter Thermostat PoC (CSA Cluster 0x0201 conformance, zero MQTT alloc when Matter active), Legacy W5500 web-only rejection of Matter, and Delta vendor features isolation to Web UI. Run complete test suites and profile validations to determine empirical APPROVE/REQUEST_CHANGES verdict.

## 🔒 My Identity
- Archetype: EMPIRICAL CHALLENGER
- Roles: critic, specialist
- Working directory: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/challenger_m5_2
- Original parent: fed7dce7-ef41-4f48-9ccd-5fd974fec9ce
- Milestone: M5
- Instance: 2 of 2

## 🔒 Key Constraints
- Review-only — do NOT modify implementation code directly (write tests/harnesses to reproduce bugs)
- Strict empirical verification — must execute tests, scripts, and harnesses directly; never trust claims without running code
- Mutually exclusive automation integration: only one automation integration (Matter OR MQTT) active at a time
- Zero MQTT heap allocation when Matter is active
- Standard CSA Cluster 0x0201 specs compliance for Matter Thermostat
- W5500 legacy builds must reject Matter initialization and remain Web-only
- Delta vendor features (Boost mode, diagnostics, object browser) stay exclusively in Web UI

## Current Parent
- Conversation ID: fed7dce7-ef41-4f48-9ccd-5fd974fec9ce
- Updated: 2026-09-02T20:40:00Z

## Review Scope
- **Files to review**:
  - `main/matter_service.c`, `main/matter_service.h`
  - `main/mqtt_service.c`, `main/mqtt_service.h`
  - `main/main.c`, `main/app_config.h`, `main/app_config.c`
  - `main/web_server.c`, `main/delta_service.c`
  - `tools/validate_build_profiles.sh`
  - `tests/run_e2e_tests.py`, `tests/`
  - `sdkconfig.*`
- **Interface contracts**:
  - `/Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/ORIGINAL_REQUEST.md`
  - `/Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/PROJECT.md`
  - `/Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/TEST_READY.md`
- **Review criteria**:
  - Mutual exclusion between Matter and MQTT
  - Heap deferred / zero alloc for inactive integrations
  - Matter Thermostat CSA Cluster 0x0201 attribute mapping & standard endpoints
  - W5500 build rejection of Matter
  - Delta vendor features isolation to Web UI
  - Build profile validity & test pass rate

## Attack Surface
- **Hypotheses tested**: [TBD]
- **Vulnerabilities found**: [TBD]
- **Untested angles**: [TBD]

## Loaded Skills
- None required for this phase.

## Key Decisions Made
- Initiated adversarial test suite creation for M5 verification.

## Artifact Index
- `BRIEFING.md` — persistent memory & identity
- `progress.md` — liveness heartbeat
- `DISPATCH.md` — received instructions log
