# Backlog

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

- **HomeKit multi-device naming**: investigate why adding the device to HomeKit
  installs as a single "Matter Accessory" tile rather than being split into
  individual named tiles per object (contradicts earlier assertion this was
  a Google Home quirk). Determine if HomeKit supports per-object naming or
  if there's a discovery/configuration path to expose multiple accessories
  rather than one aggregate device.
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
