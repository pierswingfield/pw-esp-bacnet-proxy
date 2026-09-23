# Backlog

## HTTP "Connection reset by peer" under sustained polling - root-caused (2026-09-23)

Open since 2026-09-12 ("one open HTTP-reset bug not yet root-caused" -
ping stays healthy at 7-8ms, HTTP stops accepting new connections,
confirmed persistent/non-self-resolving at the time). Root-caused live by
a second Claude session working in parallel tonight, reproduced reliably
via the object scan's status-polling (`/api/objects/scan-status` every
~4s) — died at t=38s/55% through a scan with `httpd_accept_conn: error in
accept (23)` on serial (errno 23 = ENFILE, system-wide socket table
exhausted).

Eliminated as causes (each independently confirmed, not assumed):
- **BACnet's own networking**: `bip_init()` opens exactly one UDP socket
  once at worker startup; the scan dispatches through the existing
  worker queue and never calls `socket()` itself.
- **Two httpd instances**: `start_provisioning_ap()` and
  `start_connected_webserver()` (main.c ~5580/~5700) are mutually
  exclusive within a boot (provisioning only runs when STA connect
  fails), never concurrent - only one 8-socket httpd pool ever exists.
- **espressif__mdns**: off by default (`MdnsEnabled` gates
  `mdns_start_service()`), not a factor unless a user opts in; its
  create/delete socket pairing in `mdns_networking_socket.c` also looks
  correctly guarded on every path checked.
- **Matter's entire networking stack**: `UDPEndPointImplLwIP.cpp`
  (connectedhomeip `src/inet/`) uses raw lwIP PCBs (`udp_new`/`struct
  udp_pcb`), never calls `socket()`. Matter's own minimal-mDNS
  advertisement (`src/lib/dnssd/minimal_mdns/Server.cpp`, the
  `_matterc._udp`/`_matter._tcp` advertisement) delegates to the same
  `chip::Inet::UDPEndPoint` - same conclusion. Neither Matter's main
  protocol traffic nor its own discovery touches the BSD-socket/fd table
  ENFILE exhausted, structurally.

**Likely actual cause**: not a leak anywhere in this codebase. Both httpd
instances have `lru_purge_enable=true` (main.c ~5622/~5734), so httpd's
own 8-socket pool never overflows - it evicts its own least-recently-used
connection to make room. But that only updates httpd's internal
bookkeeping; the underlying closed TCP socket still enters `TIME_WAIT` at
the lwIP/OS level independently, and lingers there regardless (standard
~30-60s). At ~4s polling, overlapping `TIME_WAIT` entries accumulate in
the shared `CONFIG_LWIP_MAX_SOCKETS=16` system-wide table faster than
they expire, stacking on BACnet's 1 permanently-open socket, until
`accept()` itself fails with ENFILE regardless of what httpd's own pool
thinks it has room for. Timing fits: ~38s at ~4s/poll is roughly the
point enough overlapping `TIME_WAIT` sockets would pile up.

**Fix options, cheapest to most involved**: slow the scan-status poll
interval; raise `CONFIG_LWIP_MAX_SOCKETS` for headroom (buys more time
before tripping, doesn't fix the underlying pattern); move scan-status
polling to a persistent connection (WebSocket/SSE) instead of repeated
short-lived GETs, which would eliminate the `TIME_WAIT` churn entirely -
this is the only option among the three that actually removes the root
cause rather than pushing the ceiling further out. Given the object scan
already needs a progress-indicator UX pass, worth doing both together.

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

- **System power/boost/health object instance numbers are hardcoded to
  this specific Delta DAC-1180E's program**: `SYS_POWER_WRITE_INSTANCE`,
  `SYS_POWER_READBACK_INSTANCE`, `BOOST_INSTANCE`, and the full
  `HEALTH_*_INSTANCE` set (~18 macros) in `main.c` are fixed constants
  tied to this one controller's verified point map ("confirmed via Phase
  0.5 diff"). Unlike room setpoint/temperature/power instances - which
  *are* per-install configurable via `rooms.json` - there is no UI or
  config path to remap these for a different Delta unit's programming, a
  different FCU controller model, or a different vendor's BACnet device
  entirely. Anyone deploying this against different controller logic
  needs a firmware rebuild, not a settings change.
  Also found while scoping this: the same constants are independently
  `#define`'d in both `main.c` and `hvac_core/include/hvac_core.h` (kept
  in sync by hand today) - any fix needs to pick one owner first, or it
  just adds a second place to forget to update.
  Feasibility: making these *configurable* (NVS-backed, same pattern as
  `rooms.json`) is straightforward - the object scan already walks
  `PROP_OBJECT_LIST` and reads names, so the read side exists. Making
  them *automatic* is not realistically feasible: BACnet has no semantic
  tag for "this MSV is the boost mode" - even this controller's mapping
  only exists because it was manually diffed against known-good/known-bad
  states (Phase 0.5). The practical version is a points-mapping UI (like
  `rooms.json`'s per-room instances) where the scan **suggests** candidates
  by matching object names against expected patterns, and a human
  confirms - not blind auto-detection.
~~**`HVAC_CORE_MAX_ROOMS` (8) and `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT`
(16) were two independent constants kept in sync by hand**~~ - fixed: a
`static_assert` in `matter_adapter.cpp` now fails the build if
`HVAC_CORE_MAX_ROOMS + 4` (aggregator + system + 2 boost switches)
exceeds the configured Matter endpoint budget, instead of silently
overflowing it at runtime.

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
