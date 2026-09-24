# Backlog

## Freeze the partition table with headroom, so future changes never need serial (2026-09-24)

Evaluated re-architecting so the device never needs a serial reflash again,
even across future partition-layout changes. Current
[partitions_t_eth_lite.csv](../firmware/bacnet_bridge/partitions_t_eth_lite.csv)
(4MB flash): `nvs` 24K, `otadata` 8K, `phy_init` 4K, `ota_0`/`ota_1` 1900K
each, `coredump` 64K - ~148K unaccounted slack, not a real reserved
partition. Current app image is ~1.15MB, so each OTA slot already has
~750K headroom for the app to *grow*, but there's no room to add a *new*
partition without shrinking the OTA slots, which means a new table.

The partition table itself lives in one unmirrored region at flash offset
`0x8000` - no A/B copy like the app slots have. OTA-updating it directly is
technically possible but brings real brick risk (power loss mid-write
leaves the bootloader unable to find any table), and this device is
headless in a wall enclosure with no easy recovery access. Not worth
building that safety net (staged write, checksum, fallback) versus the
alternative below.

Recommended approach: one more deliberate serial flash with a rebalanced
table - shrink `ota_0`/`ota_1` to ~1600K each (still 450K headroom over the
current image) and carve out an explicit ~700K `reserved` gap at the end.
Future partition needs (bigger NVS, a config blob store, etc.) get a new
table entry that lands inside that gap without moving existing offsets, so
it ships as a normal OTA app update - not a table change. This only fails
to cover: needing an OTA slot bigger than ~1600K, or needing a third OTA
slot. Do this as a dedicated pass, sizing the reserved gap against actual
current image size before locking it in.

~~**Matter DNS-SD advertiser failed to start**~~ - fixed (2026-09-24): a real
Apple Home pairing attempt got all the way through BLE PASE and several
`GeneralCommissioning` invokes, then failed with a communication error. The
device-side log showed `chip[DIS]: Failed to initialize advertiser:
3000008`, then the same family of errors (`Failed to remove advertised
services: 3`, `Failed to advertise commissionable node: 3`, later
`Operational advertising failed: 3`) on every boot, immediately - not a
heap-decays-over-time thing, a first suspicion (moving `CONFIG_MDNS_TASK_
CREATE_FROM_SPIRAM`/`CONFIG_MDNS_MEMORY_ALLOC_SPIRAM` on, since mDNS
defaulted to internal-only RAM) that turned out not to be it: same failure
after that change. Root cause, confirmed by decoding the error and by a
live A/B test: `3000008` is `0x03000008` - top byte `0x03` is CHIP's
`Range::kLwIP`, low 24 bits `8` is lwIP's `ERR_USE` (address/port already
in use). This bridge's own mDNS responder (`esp-bacnet-bridge.local`,
`mdns_start_service()` in `main.c`) and Matter's own DNS-SD advertiser both
go through the same shared `esp_mdns` component/socket - with both trying
to run, Matter loses. Disabling the bridge's own mDNS toggle made pairing
work immediately (user-confirmed live). Fixed properly rather than by
convention: `mdns_apply_setting()` now refuses to start the bridge's own
responder whenever Matter is the active integration, regardless of the
saved preference, and re-applies on every integration-mode change; `POST
/api/device/mdns` rejects turning it on while Matter is active with a
clear error; `/api/network` reports `mdns_matter_conflict` so the Update
page greys the toggle out with an explanation instead of silently no-oping
it. This is a real, permanent trade-off while Matter is selected (not just
during the pairing window - the same failure re-triggers on ordinary
post-pairing operational re-advertising too), not a narrower one worth
trying to shrink: reach the bridge by IP instead, same fallback mDNS-off
installs already needed.

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
- If the Controller Analysis step has already run (re-entering the wizard
  after a previous pass), don't re-run it by default - show the existing
  results and offer a "re-run analysis" option instead.
- **Bug: re-running Controller Analysis duplicates rooms.** Went from 2 rooms
  to 5 after a re-run; each of the 3 new rooms appears to carry the
  bedroom's BACnet data rather than its own. Likely an append-instead-of-
  replace when the analysis result is merged into existing room config -
  worth checking alongside the re-run item above since re-run behaviour is
  clearly not idempotent right now.
- **Bug: Status page shows "Target device: Not confirmed yet"** even though
  the target should be configured. Trigger unclear - seen fresh, not yet
  correlated to a reboot (a reboot-revert case for this exact field was
  already fixed, see `main.c` around `target_config_load()`) or to a wizard
  re-run. Renders from `s.bacnet_target_name` being empty in
  [status.html:115](../firmware/bacnet_bridge/main/status.html:115) - check
  whether discovery is actually failing/not being confirmed, or whether the
  name is being lost somewhere after a successful discovery (possibly same
  root cause as the room-duplication bug above, if wizard re-runs are
  clobbering NVS state generally).

## Privacy policy

- Published policy covering what the telemetry and cloud admin items above
  collect, store and retain - including BACnet object data such as room
  temperatures, names and configuration. Gate for shipping telemetry or
  remote administration, not a follow-up.
