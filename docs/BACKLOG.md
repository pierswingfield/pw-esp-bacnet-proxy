# Backlog

## Hardcoded-assumption sweep (2026-09-23)

Prompted by finding the room-config truncation bug while stress-testing
Matter at 6 rooms: swept for other places where a specific-install
assumption or fixed limit isn't actually configurable per the project's
own protocol-agnostic, scalable-by-design intent. Three silent-truncation
bugs of the same shape (object scan, custom MQTT points, BACnet discovery)
were found and fixed directly, and three of the five architectural items
below were subsequently addressed too (BACnet discovery's scan range,
the bridge's static IP, mDNS hostname collisions) - see their commits for
detail. The remaining two are real but are design decisions needing their
own dedicated pass, not one-line fixes.

~~**BACnet discovery hardcoded to a `10.0.3.x` unicast scan range**~~ -
fixed: `execute_worker_discovery()` now derives the scan range from the
bridge's own live IP (`esp_netif_get_ip_info`) instead of a hardcoded
subnet.

~~**The bridge's own static IP was compiled in**~~ - fixed: Ethernet
bring-up now tries DHCP first (5s), falling back to the historical
static config only if nothing answers - a differently set up segment
that runs DHCP now gets a correct address automatically.

~~**mDNS hostname was a single fixed string**~~ - fixed: probes for an
existing responder on the default name before claiming it, falls back
to `-1`, `-2`, etc. on collision.

~~**System power/boost/health object instance numbers were hardcoded to
this specific Delta DAC-1180E's program**~~ - fixed: `hvac_core` now owns
all 21 whole-unit points (the duplicate `SYS_POWER_*` defines in `main.c`
and `hvac_core.h` are gone). The wizard scans the controller, matches
object names against per-point patterns in the browser, proposes confident
matches with a score and flags the rest as missing; the installer confirms
or overrides and the map is stored in `nvs_points`. Still fixed per-program:
the Boost state numbers (1/4/5), the fan supply-air/speed instance lists and
the alarm binary-values used by the Health page.

- **System power command (BV13) reverts to on by itself** (observed
  2026-09-24 on the test device, Matter build with the confirmed point map).
  A dashboard "Turn Off" wrote BV13=0 (`system-power value=off ok=1`, read
  back 0), then BV13 returned to 1 about 30 s later with no HTTP write, no
  incoming Matter Invoke/Write and no MQTT on the bridge; the Matter mirror
  only followed the change. BV13 has no priority array, so the last writer
  wins. Not caused by the point-mapping change (same object, same write
  path). Next: watch BV13 from a BACnet tool with the bridge idle, to tell
  controller program logic apart from another BACnet client.

- **`HVAC_CORE_MAX_ROOMS` (8) and `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT`
  (16) are two independent constants that must be kept in sync by hand**:
  at 8 rooms the Matter side needs aggregator(1) + system(1) + boost(2) +
  rooms(8) = 12 of the 16 available dynamic endpoints. Raising
  `HVAC_CORE_MAX_ROOMS` alone in the future (e.g. to 12, for a larger
  home) would silently exceed the Matter endpoint budget at 16 rooms'
  worth of config without any build-time check tying the two together -
  worth a `static_assert` or Kconfig cross-check rather than relying on
  whoever changes one constant remembering to check the other.

## Automatic updates

- Scheduled background check of the T-ETH-Lite release manifest, plus a
  device setting selecting the behaviour: off, auto-check only (notify that
  an update is available), or auto-update (download and install
  unattended). Today both the check and the install are user-initiated from
  the Update page, so this changes the documented OTA contract and needs
  the rollback path exercised before auto-update is offered.

## Telemetry and cloud admin panel

- Opt-in outbound telemetry to a hosted admin panel for fleet-wide
  visibility. Scope is open-ended: device health (uptime, reset reason,
  heap and stack headroom, BACnet link health) through to BACnet object
  data itself - room temperatures, setpoints, room names and
  configuration, object catalogue. Today all of it is readable only
  per-device over LAN HTTP. Room temperatures and names are
  occupancy-revealing, so the collection scope and retention have to be
  settled alongside the privacy policy below rather than after.

## Remote administration

- Administering a bridge from outside its LAN. The current model is
  LAN-only, authenticated HTTP with the device on an isolated `10.0.3.x`
  BACnet segment, so this needs an explicit exposure design
  (outbound-initiated tunnel vs inbound port) rather than reusing the
  existing admin endpoints as-is.

## Matter integration

- **Generic "Matter-enabled accessory" label while scanning**: Google Home
  showed this generic label at the pre-commissioning scan step, unrelated
  to the per-endpoint names set after pairing. Root-caused via a research
  pass through connectedhomeip source: `CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME`/
  `_PRODUCT_NAME` only populate the Basic Information cluster's attributes,
  which a controller can only read *after* commissioning
  (`GenericDeviceInstanceInfoProvider::GetVendorName/GetProductName`,
  consumed by `src/app/clusters/basic-information/basic-information.cpp`)
  - proven by grep, not inferred. The DNS-SD commissionable-node
  advertisement (what the scan screen actually reads) has its own "DN"
  (device name) TXT key, populated by `DnssdServer::Advertise()`
  (`src/app/server/Dnssd.cpp`) only when
  `CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONABLE_DEVICE_NAME` is set - off by
  default, and we were never setting it. Fixed in `chip_project_config.h`
  by enabling that macro and setting `CHIP_DEVICE_CONFIG_DEVICE_NAME`.
  Unproven: whether Google's scan UI actually renders the `DN` value
  instead of its fallback string - that's client-side rendering we can't
  inspect from firmware source; needs a live re-scan to confirm.
- **HomeKit multi-device tiling**: Google Home already splits our 5 flat
  sibling endpoints (no Aggregator parent) into separate device cards on
  its own. Whether HomeKit needs a literal Aggregator endpoint (device
  type `0x000E`) + per-child `BridgedDeviceBasicInformation` to do the
  same - the pattern Espressif's own `examples/bridge_apps/zigbee_bridge`
  uses - is a real, spec-legitimate structural change, but HAP's
  client-side rendering logic isn't in the connectedhomeip source tree,
  so the payoff for HomeKit specifically is unproven without live testing
  on real Apple hardware. Restructuring under an Aggregator would also
  affect Google Home's current (working) tiling, so treat as a
  higher-risk change to test in isolation, not a quick fix.
- **Matter QR code generation**: generate and display the Matter setup QR code
  on the device web UI to simplify HomeKit/Google Home pairing. Needs to
  encode the setup payload and render dynamically on the configuration page.
- **Tidy up the Matter config page**: the Matter section of the Smart Home
  page (`smart_home.html`) was carried over largely as-is from the original
  single-purpose `/smart-home` page; review layout, copy and information
  density now that it sits inside the merged module-switcher page rather
  than as a standalone destination.

## Object scanning UX

- The Objects page scan gives no progress indication beyond the raw
  percentage text and doesn't warn the user up front that the web UI
  becomes unresponsive while a scan runs (it walks the controller's full
  object list over BACnet, one ReadProperty per object, and the HTTP task
  is busy driving that). Add a visible progress bar and a clear warning
  before the scan starts, not just a number that updates via polling.

## Firmware release consolidation

- Deprecate the existing `master` branch build (single-room, MQTT-only,
  smaller partition table) and promote this modular build
  (`codex/protocol-agnostic-core` - protocol-agnostic `hvac_core`, Matter
  support, multi-room) to be the new `master`. Note: the two use different
  partition tables and flash-size assumptions (master: 4MB/1900K OTA slots;
  modular: 16MB/4MB OTA slots) - existing master-branch devices need a full
  serial reflash, not an OTA update, to move to the new build (see chat
  history for the detailed compatibility check).

## Health/Status page reorganisation

- Remove the automation-mode display/control from the Health page and move
  it to the Status page instead - Health should stay diagnostic-only
  (matches the existing Smart Home IA principle already applied elsewhere).
- Rename the Update page to "System" (nav label, page title, and its
  `<h1>`) - it already covers OTA, mDNS and backup/restore, not just
  updates, and now also OTA password management.

## Privacy policy

- Published policy covering what the telemetry and cloud admin items above
  collect, store and retain - including BACnet object data such as room
  temperatures, names and configuration. Gate for shipping telemetry or
  remote administration, not a follow-up.
