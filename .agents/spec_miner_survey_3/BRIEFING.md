# BRIEFING — 2026-09-02T20:07:00Z

## Mission
Investigate and specify the HVAC Core architecture, canonical room model, NVS schema, single active automation integration mutual exclusion, and Matter Thermostat endpoint requirements for ESP-BACnet.

## 🔒 My Identity
- Archetype: Specification Miner
- Roles: Teamwork specialist, Specification Miner
- Working directory: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/spec_miner_survey_3
- Original parent: fed7dce7-ef41-4f48-9ccd-5fd974fec9ce
- Milestone: Survey & Specification Phase

## 🔒 Key Constraints
- Read-only on product code: Do NOT implement anything during specification mining.
- Authoritative sources: Probe actual codebase, IDF configs, ESP-Matter SDK, headers, and specifications.
- Dual-profile requirement: W5500 legacy profile must have zero Matter dependencies.
- Single source of truth: components/hvac_core must own canonical room model, active room count, BACnet mapping, and NVS schema.
- Mutual exclusion: MQTT and Matter are mutually exclusive; inactive transports must not allocate tasks/queues/buffers.
- Hardware boundary: T-ETH-Lite (ESP32-WROVER-E 16MB flash, 8MB PSRAM) is target for Matter; W5500 is 4MB flash, no PSRAM.
- Do NOT modify bacnet-object-catalog.json.
- Do NOT flash physical hardware.

## Current Parent
- Conversation ID: fed7dce7-ef41-4f48-9ccd-5fd974fec9ce
- Updated: 2026-09-02T20:07:00Z

## Task Summary
- **What to build**: Specification report and data contracts for HVAC Core, Single Active Integration, and Matter Thermostat endpoint.
- **Success criteria**: Comprehensive handoff.md detailing data models, BACnet mapping, NVS schema, mutual exclusion state machine, Matter cluster attributes, and build dependency decoupling.
- **Interface contracts**: ORIGINAL_REQUEST.md
- **Code layout**: components/hvac_core, firmware/bacnet_bridge/main/

## Key Decisions Made
- Discovered and mapped all 20 core HVAC/Matter features and 8 critical edge cases.
- Formalized NVS schemas (`nvs_rooms`, `nvs_integ`) and mutual exclusion rules.
- Specified CSA Matter Cluster 0x0201 Thermostat attributes and vendor confinement.
- Verified zero Matter dependency requirement for W5500 legacy profile.

## Artifact Index
- DISPATCH.md — Assignment instructions
- progress.md — Liveness & heartbeat
- handoff.md — Final specification report
