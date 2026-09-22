# System Power / room power decoupling — implementation record

Status: **implemented on both branches** (`master`, `codex/protocol-agnostic-core`).
Written 2026-09-22. Revised 2026-09-22 after bench feedback reversed the
original cascade approach — see §4 "Revision history" before reading the
edits below as current.

Applies to **both** branches: `master` and `codex/protocol-agnostic-core`.

---

## 1. The problem, in one paragraph

The HA "System Power" switch writes BACnet `binary-value:13`
(`SYS_POWER_WRITE_INSTANCE`, the BMS Run Signal). That write physically turns
the FCU off — confirmed on live hardware. The switch's *reported state* used
to come from a different point, `binary-value:1` (`SYS_POWER_READBACK_INSTANCE`,
"FCU Run Status"). That point is not a bug — [`objects.html:337`](../firmware/bacnet_bridge/main/objects.html)
documents it honestly: "Whether the unit is actually running. A wall panel or
PIR can keep it running after the BMS says stop, so this will not always
match System power." It answers a different question ("is anything actually
running") than the one the switch needs to answer ("did we tell it to stop").
Using it to drive the switch's on/off display made the switch lie about what
was commanded whenever something else kept the FCU alive.

## 2. The decision (final)

**System power and room power are functionally independent by design.**
Turning System Power off must never write to any room's power point — the
same room configuration has to survive a system-off/on cycle with zero
re-configuring. What needed fixing was only the *display*:

| Signal | Source | Meaning | Used for |
| --- | --- | --- | --- |
| Raw run status | `binary-value:1` read | "Is anything actually running" (can diverge — by design, not a bug) | `manage.html` "FCU run status (readback)" row only. Left completely alone. |
| Commanded state | `binary-value:13` read | "What did we last tell it to do" | System Power switch/toggle everywhere: web UI, HA discovery, MQTT `system_power/state`, `/api/status`'s `sys_power_commanded`. |
| Room power | Per-room `binary-value` | The room's own configured on/off | Room cards, HA room `climate.mode`. Never touched by the system switch. |
| Room action | Derived: room power AND system power commanded | "Is this room actually conditioning right now" | HA `hvac_action` topic and the web UI's `(Idle - system off)` qualifier. |

BV:13 was already the point the bridge writes, so reading it back is a
direct, reliable report of the last command — no extra bookkeeping, no room
involvement, no divergence risk from a wall panel or PIR.

## 3. What actually shipped

**`hvac_core_set_system_power()` / the equivalent master handler**: writes
BV:13 only. No room writes, ever.

**`hvac_core_get_system_power_commanded()`** (codex) /
**`system_power_commanded()`** (master): reads BV:13's own present-value.
Replaces every place that used to read BV:1 or derive from room state for
the switch: the on-device toggle, HA discovery's initial state, the MQTT
`system_power/set` handler's republish, and the poll-burst publish to
`system_power/state`.

**`hvac_core_get_system_power()`** (codex) / the raw read in `/api/status`
(master): unchanged, still reads BV:1, still only feeds the "FCU run status
(readback)" label.

**Room action derivation** (`mqtt_publish_room_action()`, both branches):
if the room's power point is off, publish `"off"` (unchanged). If the room's
power point is **on** but system power is **not commanded on**, publish
`"idle"` instead of computing from thermal output — the room's own `mode`
topic is untouched, only its `action` goes idle.

**`/api/status`**: gained `sys_power_commanded` / `sys_power_commanded_valid`
alongside the pre-existing raw `sys_power` / `sys_power_valid`. See
`docs/ARCHITECTURE_AND_API.md` §6 for the field reference.

**`manage.html`**: the System Power toggle button's label/state now reads
`sys_power_commanded`, not the raw readback (previously it silently reused
the same value as the honest raw row, which meant the button could get
stuck showing "Turn Off" even after the FCU had genuinely stopped). Room
cards show `On (Idle - system off)` when a room's power point is on but
system power is commanded off.

## 4. Revision history

The first version of this plan cascaded room power off whenever system
power turned off (and derived the switch's state from whether any room was
still on). That was implemented, built, and committed on both branches, then
**reverted** after clarifying the actual requirement: room power and system
power are meant to be independently configurable, specifically so a
System-Power-off/on cycle doesn't disturb which rooms were running. The
cascade broke that. The commit that reverted it also fixed the real bug —
sourcing the switch from BV:13 instead of BV:1 or room state — in the same
change. Nothing in §2–3 above reflects the cascade; it's gone.

## 5. Home Assistant side — outside this repo

No YAML change was needed. The System Power switch in HA is a bare
MQTT-discovery entity created straight from the firmware; it already
subscribes to `system_power/state`, which now carries the corrected
(BV:13-derived) value. The separate "Home Cooling" combined climate entity
(`configuration.yaml:519`, `apartment_combined_climate_*` automations)
mirrors the two room climate entities' `hvac_mode` and has nothing to do
with system power — do not touch it for this.

## 6. Build and verify

Source ESP-IDF 5.3.1 first.

```bash
cd firmware/bacnet_bridge && idf.py -B build-t-eth -DSDKCONFIG=build-t-eth/sdkconfig -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.t_eth_lite.defaults' build
```

Then, before any release:

```bash
tools/validate_build_profiles.sh
```

Both passed clean on both branches for this change.

**A clean build is not evidence this works.** Per `CLAUDE.md`, a hardware
change needs live evidence naming the flashed image and the observed result.
Required live checks, on hardware:

1. Rooms configured on, system power on → flip System Power off in HA or the
   web UI → the switch/toggle itself reads off (not stuck "on"); room cards
   keep showing "On" but with the `(Idle - system off)` qualifier; HA rooms'
   `hvac_action` reads idle; FCU physically stops.
2. Flip System Power back on → rooms resume exactly as configured, with zero
   manual re-toggling.
3. `manage.html` "FCU run status (readback)" still shows the raw BV:1 value,
   whatever it says, unaffected by any of the above.

Bench-tested informally against a build from commit `48993a2`
(`codex/protocol-agnostic-core`) 2026-09-22 — "seems fine" per user
feedback. Formal per-check sign-off against the list above still open.

Only one bridge may sit on the isolated `10.0.3.x` BACnet segment at a time
— both profiles use the same static address.

## 7. Scope guard

Do **not**, in this change:

- rename or repurpose BV:1 / `SYS_POWER_READBACK_INSTANCE`
- remove the System Power switch from HA discovery
- touch `bacnet-object-catalog.json` (user-generated commissioning output)
- hardcode a room count anywhere
- write to any room's power point from the system-power write path
- refactor the surrounding MQTT publish code beyond the derivation swap
