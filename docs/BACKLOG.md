# Backlog

## Matter DNS-SD advertiser can fail to start under low internal heap (2026-09-24)

Observed live on the test device (T-ETH-Lite, Matter build, 21-point map
confirmed, 2 rooms): right after `matter_adapter_start()` finishes creating
endpoints, internal heap was down to ~6.6KB with a largest free block of
~2.3KB. `chip[DIS]` then logged `Failed to initialize advertiser: 3000008`,
`Failed to remove advertised services: 3`, `Failed to advertise
commissionable node: 3`, `Failed to finalize service update: 3`. The rest of
Matter came up fine (`matter_active`/`running`/`window_open` all true,
endpoints live, thermostat clusters report), so this isn't fatal - but a
failed DNS-SD advertisement means the device may not appear in a phone's
Matter "add device" scan even while otherwise healthy, and there was no
retry observed. A second boot (same firmware, no other changes) may or may
not hit it - internal heap headroom at this exact moment depends on WiFi/
Ethernet/BACnet worker startup timing, so it's a timing-sensitive resource
race, not a deterministic failure. Matches the long-standing heap-budget
story for this board (see the ESP32 heap-budget and heap/stack-budget
memory notes) - BACnet + Ethernet + WiFi + HTTP + Matter concurrently is
close to this board's internal-RAM ceiling. Worth a retry (or a deferred
start once other subsystems have settled) around `DiscoverableAdvertiser`
startup rather than treating it as one-shot.

## Fresh `idf.py` build directories can silently pick the wrong partition table (2026-09-24)

Building a brand-new `build-t-eth` directory with the documented command
(`idf.py -B build-t-eth -DSDKCONFIG=build-t-eth/sdkconfig
-DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.t_eth_lite.defaults'
build`) produced a resulting sdkconfig with
`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"` (the old
master-branch 1900K-slot table) instead of
`sdkconfig.t_eth_lite.defaults`'s `partitions_t_eth_lite.csv` (4MB slots) -
even though the later file in `SDKCONFIG_DEFAULTS` should override the
earlier one for the same key. Flashing that image over USB (bootloader +
partition table + app) silently rewrote the device's flash layout to the
small-slot table, and a subsequent OTA of a normal ~2.4MB T-ETH-Lite/Matter
image then failed with `esp_ota_begin failed: ESP_ERR_INVALID_SIZE` because
it no longer fit. A pre-existing build directory (e.g.
`build-t-eth-matter` under `~/esp-bacnet-nospace`, built correctly in an
earlier session) was unaffected - only fresh directories with both default
files passed together were seen to have this happen. Not yet root-caused
(kconfig default-merge ordering, or something order-dependent in how the
two files' custom-filename choices interact); reproduce deliberately in an
isolated build dir before trusting this documented command for a from-
scratch T-ETH-Lite build again, and verify
`grep CONFIG_PARTITION_TABLE_CUSTOM_FILENAME <builddir>/sdkconfig` picked
`partitions_t_eth_lite.csv` before flashing.

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

## Project naming

- Rename `esp-bacnet-setup` (the onboarding Wi-Fi AP SSID) and the project
  name itself across the codebase - currently named for the original
  BACnet-only scope, predates the protocol-agnostic/Matter/MQTT direction.

## Onboarding wizard

- Rename the "Proceed to MQTT" button to "Proceed to smart home setup" -
  wizard now covers more than MQTT.
- Add a "None / set up later" option so the user can skip configuring any
  smart-home module during onboarding - check whether this already exists
  before implementing.
- More in-wizard guidance when the user chooses Matter setup. Immediate gap:
  the Footer needs a reboot button (and a backing API endpoint) - currently
  there's no way to reboot from that step of the flow.

## Privacy policy

- Published policy covering what the telemetry and cloud admin items above
  collect, store and retain - including BACnet object data such as room
  temperatures, names and configuration. Gate for shipping telemetry or
  remote administration, not a follow-up.
