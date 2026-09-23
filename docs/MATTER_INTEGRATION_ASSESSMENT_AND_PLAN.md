# Local automation integration assessment and implementation plan

**Status:** active design and first refactor extraction on
`codex/protocol-agnostic-core`. This is not a released integration. MQTT/Home
Assistant remains the supported automation transport until the Matter gates
below pass on physical hardware.

**Update 2026-09-12:** the real ESP-Matter SDK is now linked (not a stub) and
verified live on T-ETH-Lite hardware — boots, creates a genuine Thermostat
endpoint, publishes `_matterc._udp` over mDNS, and opens a real
CASE-session-capable commissioning window. Full detail, every bug found and
fixed, and one still-open issue: `docs/MATTER_INTEGRATION_SESSION_2026-09-12.md`.
The sections below describing this as scaffolding/PoC-only are now historical;
kept for the reasoning trail, not as current status.

## Decision

Use **Matter-over-Wi-Fi** as the one direct, local external-integration module
for Apple Home, Google Home and Alexa. Keep MQTT/Home Assistant as a separate,
selectable module. Do not build three proprietary integrations.

Matter is IP based, supports multi-admin sharing across ecosystems, and lets
the bridge communicate on the home LAN rather than through a service operated
by this project. Voice assistants and their user accounts remain each
ecosystem's concern; the bridge has no vendor cloud, account linking service
or hosted endpoint.

## Platform and user environment

| User choice | What the bridge implements | User-side requirement | Hub-free? |
|---|---|---|---|
| Home Assistant | MQTT plus Home Assistant Discovery | MQTT broker and Home Assistant | No broker-free path; existing option |
| Apple Home | Matter-over-Wi-Fi | iPhone/iPad with Bluetooth enabled and local Wi-Fi | Yes for iOS 18+ local pairing/control; Apple recommends a home hub |
| Google Home | Matter-over-Wi-Fi | Google account, Google Home app, IPv6-capable home Wi-Fi and Matter-enabled Google Home hub | No |
| Alexa | Matter-over-Wi-Fi | Amazon account, Alexa app and Matter-capable Echo or eero controller | No |

Apple documents iOS 18+ local Matter setup/control without a home hub, while
remote and guest access require one. Google documents a Matter hub as required
to set up and control Matter devices in Google Home. Alexa documents a
compatible Echo/eero as the Matter administrator. The integration is still
local to the bridge after commissioning; this does not imply offline voice
recognition or remove the consumer platform's own account requirements.

Authoritative references:

- [Apple: adding Matter accessories](https://support.apple.com/en-us/126198)
- [Apple: remote and guest access](https://support.apple.com/guide/iphone/invite-others-to-control-accessories-iphcbaf7e8f3/26/ios/26)
- [Google: Matter setup and hub requirements](https://support.google.com/googlehome/answer/13127223)
- [Amazon: local Matter connection](https://developer.amazon.com/en-GB/docs/alexa/smarthome/matter-support.html)

## Explicitly rejected approaches

- **Google Local Home SDK:** it is not an embedded ESP32 substitute for Matter
  and depends on Google's smart-home action/cloud fulfilment model.
- **Alexa Smart Home Skill:** requires a skill/cloud backend, contrary to the
  no-hosting requirement.
- **A native HAP-only integration:** technically possible through Espressif's
  HomeKit SDK, but duplicates Apple-only work that Matter already covers and
  creates a second external model. It is not planned unless Matter proves
  insufficient for a required Apple-specific workflow.

## Library and device-model choice

Use [Espressif ESP-Matter](https://github.com/espressif/esp-matter),
Espressif's open-source Matter framework for ESP32-series devices. Model every
active configured room as a standard Matter **Thermostat** endpoint. Map only
portable capabilities:

- current temperature;
- occupied setpoint;
- enabled/off state where the controller mapping is semantically sound; and
- supported heat/cool/auto modes once their BACnet semantics are verified.

Keep boost, whole-unit diagnostics, object browsing, unusual Delta states and
other vendor-specific behaviour in the local web dashboard. Non-standard
Matter clusters do not reliably produce native controls in Apple, Google or
Alexa user interfaces.

## Hardware and resource decision

### T-ETH-Lite is the Matter target

The primary board is classic ESP32-WROVER-E with 16 MiB flash, 8 MiB PSRAM,
integrated RTL8201 Ethernet and Wi-Fi/BLE. The current profile routes ordinary
large allocations and Wi-Fi/LwIP buffers to PSRAM while retaining a 32 KiB
internal reserve for DMA/internal-only allocations. It is the only profile
with a credible Matter resource budget.

### W5500 is excluded

The W5500 recovery board is classic ESP32-WROOM with 4 MiB flash and no PSRAM.
Its historical heap stability work makes it unsuitable for an additional
Matter/BLE stack. It remains a buildable MQTT/web recovery profile.

### Partition and measurement gates

The current T-ETH partition table uses two 1,900 KiB OTA app slots despite the
board's 16 MiB physical flash. The pre-refactor build artifact was 1,514,720
bytes, leaving about 431 KB in a 1,945,600-byte slot. A Matter-capable T-ETH
profile therefore needs larger OTA slots; no decision is to be inferred from
flash size alone.

Before enabling Matter in the wizard, measure the selected-mode build with:

1. ESP-Matter, BLE commissioning, BACnet, Wi-Fi, HTTP and diagnostics active;
2. current application size and OTA-slot reserve;
3. internal heap, minimum internal heap and largest contiguous internal block;
4. every task's stack headroom;
5. BACnet command/readback correctness during Matter subscriptions; and
6. an interoperability and soak matrix: Apple, Google and Alexa pairing,
   local control, reconnect, reboot, 15-minute and 24-hour operation.

MQTT must be disabled by default in a Matter-selected image/runtime mode so
both heavy integration stacks are not assumed to coexist.

## Target architecture

```text
BACnet worker + HVAC core + bounded commands/state
                    ├── web dashboard       (always available)
                    ├── mqtt_home_assistant (selected optional transport)
                    ├── matter              (selected direct transport)
                    └── none                (dashboard only)
```

The core owns room-to-BACnet mapping, room activation, setpoint/temperature/
power semantics and persisted room configuration. Transports may:

- translate core state to their protocol;
- submit bounded semantic commands to the core; and
- receive state-change reports from the core.

They must not persist another room mapping or perform their own direct BACnet
transactions. The next core extraction must replace cross-task direct BACnet
calls with the already-planned single BACnet worker queue; this is needed for
bounded Matter callbacks as well as MQTT and HTTP safety.

## Setup wizard end state

After Wi-Fi, target discovery and room selection, the wizard will present:

1. **Home Assistant / MQTT** — retain the existing broker and discovery form.
2. **Apple Home, Google Home or Alexa (Matter)** — enable pairing mode and
   display a per-device Matter QR/manual code; users commission from their
   chosen platform app.
3. **Dashboard only** — do not start an external automation transport.

The wizard must not request Apple, Google or Amazon credentials. Matter
commissioning needs BLE and unique setup material. A distribution-quality
device also needs per-device DAC/PAI/CD factory data, a Vendor/Product ID and
Matter certification; development credentials are not a secure production
substitute. Local LAN OTA can remain the project's update mechanism without a
hosted Matter OTA service.

## Changes completed on this branch

- Added `components/hvac_core`, a protocol-neutral component exposing
  `hvac_room_config_t`, the shared active-room model and its existing
  `nvs_rooms` persistence schema.
- Updated `main.c` to use the shared model. Existing MQTT topic formation,
  Home Assistant discovery, UI APIs and saved room configuration remain
  behaviourally unchanged.
- Declared the three intended integration selections:
  `none`, `mqtt_home_assistant` and `matter`.
- Added backwards-compatible persisted integration selection. Existing devices
  default to MQTT; non-MQTT selections defer MQTT task/client startup. No UI
  exposes Matter selection yet, so an incomplete Matter mode cannot be chosen
  through normal setup.
- Routed existing web and MQTT setpoint, room-power and system-power commands
  through shared semantic HVAC command helpers. The helpers currently retain
  the proven synchronous BACnet implementation; replacing that implementation
  with a single worker queue is the next concurrency-focused extraction.
- Added `INTEGRATION_MODULES.md` as the concise component boundary reference.
- Built the T-ETH-Lite profile successfully after this extraction. Build output
  reported application size `0x1730d0`, smallest app partition `0x1db000`,
  and `0x67f30` (22%) free.

No firmware was flashed and no new live hardware validation was performed for
this refactor.

## Ordered implementation plan

1. Complete the core command/state interface and route web/MQTT through a
   single BACnet worker queue, with regression tests for current MQTT behaviour.
2. Add a T-ETH-only Matter build profile: ESP-Matter dependency, BLE,
   factory-data development configuration and enlarged OTA partitions.
3. Build one active-room Thermostat proof of concept with read/report and
   setpoint command/readback.
4. Validate resource measurements and pairing with Apple, Google and Alexa;
   fix issues before adding a second endpoint.
5. Scale to configured rooms, add mode selection persistence and wizard UI.
6. Complete factory identity, certification and release documentation before
   describing Matter as plug-and-play for distributed hardware.
