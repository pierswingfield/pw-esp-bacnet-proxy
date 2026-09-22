# System Power cascade + derived run status — implementation handoff

Status: planned, not implemented. Written 2026-09-22.

Apply to **both** branches: `master` and `codex/protocol-agnostic-core`.

---

## 1. The problem, in one paragraph

The HA "System Power" switch writes BACnet `binary-value:13`
(`SYS_POWER_WRITE_INSTANCE`, the BMS Run Signal). That write physically turns the
FCU off — confirmed on live hardware. But the switch's *reported state* comes from
a different point, `binary-value:1` (`SYS_POWER_READBACK_INSTANCE`, "FCU Run
Status"), which stays `true` because the per-room power points were never touched.
Result: the unit is off, but the ESP dashboard says "running" and the combined HA
entity stays `on`.

Two independent defects:

- **Defect A — no cascade.** `set_system_power(off)` writes only BV:13. Per-room
  power points keep their own state, so the next room toggle restarts the unit
  with no re-assertion of System Power.
- **Defect B — status sourced from an unreliable point.** BV:1 is documented in
  `firmware/bacnet_bridge/main/objects.html:337` as able to diverge from BV:13.
  Deriving the switch state from it produces the wrong answer.

## 2. The decision

**Fix both. Cascade OFF only, and derive the HA switch state from room power.**

| Decision | Value | Why |
| --- | --- | --- |
| Cascade direction | **OFF only, never ON** | Master-off is a safe, expected action. Master-**on** forcing every room on is not what a user wants — it would fight per-room scheduling and turn on unoccupied rooms. |
| Rooms touched | **`active` rooms only**, bounded by `RoomCount` | Inactive rooms are not enabled in the setup wizard and their instance numbers may be placeholders. |
| Room count | **Always `RoomCount` at runtime** | Default is 5, max is 8 (`MAX_ROOMS` / `HVAC_CORE_MAX_ROOMS`). Other apartments have different counts. **Never hardcode 2 or 5.** |
| HA switch state source | **OR of active room power** | Reflects what the user commanded and what the rooms are actually doing. Self-consistent once the cascade lands. |
| Raw BV:1 display | **Leave completely alone** | `manage.html:115` labels it "FCU run status (readback)". That is an honest raw-point display and stays raw. Do not change it, do not rename it. |

## 3. Shared helper — add to both branches

The branch aliases `Rooms`/`RoomCount` to the hvac_core globals
(`main.c:285-286`), so this **exact same code** compiles on both branches.

```c
/* BV:13 (System Power) does not gate the per-room power points: the FCU keeps
 * each room's last commanded state, so a stale room switch silently restarts the
 * unit. Cascade OFF only - a master ON must not force unoccupied rooms on. */
static bool any_active_room_power_on(void)
{
    if (!BacnetReady) return false;
    for (size_t i = 0; i < RoomCount; i++) {
        if (!Rooms[i].active) continue;
        bool on = false;
        if (read_bool_property(OBJECT_BINARY_VALUE, Rooms[i].power_instance,
                               PROP_PRESENT_VALUE, &on) && on) {
            return true;
        }
    }
    return false;
}

static void cascade_rooms_off(void)
{
    if (!BacnetReady) return;
    for (size_t i = 0; i < RoomCount; i++) {
        if (!Rooms[i].active) continue;
        write_bool_property(OBJECT_BINARY_VALUE, Rooms[i].power_instance,
                            PROP_PRESENT_VALUE, false);
    }
}
```

Place both above the first call site on each branch. On the branch, prefer
`hvac_core_set_room_power(i, false)` / `hvac_core_get_room_power(i, &on)` inside
`hvac_core.c` rather than raw `write_bool_property` — see step 4B.

> **Heap/stack note.** These loops add up to 8 sequential BACnet reads to code
> paths that already do ~20 per poll burst. Do not add a new task, do not add a
> new buffer larger than a few bytes, and do not call `any_active_room_power_on()`
> more than once per publish site. This device has a tight heap budget.

## 4A. `master` — exact edits

All edits are in `firmware/bacnet_bridge/main/main.c`. Line numbers are from
commit `46d4f68`; re-grep before editing, do not trust them blindly.

| # | Line | Current | Change |
| --- | --- | --- | --- |
| 1 | ~2991 | HTTP `/api/system-power` writes BV:13 only | After a successful `off` write, call `cascade_rooms_off()` |
| 2 | ~6265 | MQTT `system_power/set` writes BV:13 only | Same — cascade on `off` |
| 3 | ~6268 | `mqtt_republish_bool(SYS_POWER_READBACK_INSTANCE, ...)` | Publish `any_active_room_power_on() ? "ON" : "OFF"` instead |
| 4 | ~6586 | Poll-burst publish to `system_power/state` reads BV:1 | Same swap to the derived value |
| 5 | ~4904 | Discovery-time `sys_pwr_ok` initial state reads BV:1 | Same swap to the derived value |
| 6 | ~2314 | `/api/status` `sys_power` reads BV:1 | **No change.** Raw readback, feeds the honest raw display. |

**Edit 1 shape:**

```c
    bool on = strcmp(value_str, "on") == 0;
    bool ok = BacnetReady && write_bool_property(
        OBJECT_BINARY_VALUE, SYS_POWER_WRITE_INSTANCE, PROP_PRESENT_VALUE, on);
    if (ok && !on) cascade_rooms_off();
    diag_log("system-power value=%s ok=%d", value_str, ok);
```

**Edit 2 shape:**

```c
        if (BacnetReady) {
            write_bool_property(OBJECT_BINARY_VALUE, SYS_POWER_WRITE_INSTANCE, PROP_PRESENT_VALUE, on);
            if (!on) cascade_rooms_off();
        }
```

**Edit 3 is the easy one to miss.** `mqtt_republish_bool()` re-reads BV:1 and
publishes it, which would immediately overwrite the correct state with the stale
one. Replace the call with a direct publish of the derived value.

## 4B. `codex/protocol-agnostic-core` — exact edits

The cascade belongs **inside hvac_core**, because both the HTTP handler
(`main.c:2255`) and the MQTT handler (`main.c:5509`) already route through
`hvac_command_system_power()` → `hvac_core_set_system_power()`. One edit covers
both callers.

| # | File / line | Change |
| --- | --- | --- |
| 1 | `components/hvac_core/hvac_core.c:201` | In `hvac_core_set_system_power`, after the BV:13 write succeeds and `on == false`, loop `hvac_core_get_room_count()` and call `hvac_core_set_room_power(i, false)` for each `HvacRooms[i].active` room |
| 2 | `components/hvac_core/include/hvac_core.h` | Add `bool hvac_core_any_room_power_on(void);` |
| 3 | `components/hvac_core/hvac_core.c` | Implement it: loop active rooms, `hvac_core_get_room_power(i, &on)`, return true on first `on` |
| 4 | `main/main.c:5510` | Replace `mqtt_republish_bool(SYS_POWER_READBACK_INSTANCE, ...)` with a publish of `hvac_core_any_room_power_on()` |
| 5 | `main/main.c:5839-5846` | Poll-burst publish to `system_power/state` — swap BV:1 read for `hvac_core_any_room_power_on()` |
| 6 | `main/main.c:4127-4134` | Discovery `sys_pwr` / `sys_pwr_ok` initial state — swap to the derived value |
| 7 | `main/main.c:1583-1600` | `/api/...` raw `sys_power` JSON — **no change** |

**Edit 1 shape:**

```c
bool hvac_core_set_system_power(bool on)
{
    bool ok = bacnet_worker_write_bool(OBJECT_BINARY_VALUE, SYS_POWER_WRITE_INSTANCE,
                                       PROP_PRESENT_VALUE, on);
    /* BV:13 does not gate the room points - a stale room switch would restart the
     * unit. Cascade OFF only; a master ON must not force unoccupied rooms on. */
    if (ok && !on) {
        for (size_t i = 0; i < HvacRoomCount; i++) {
            if (!HvacRooms[i].active) continue;
            hvac_core_set_room_power(i, false);
        }
    }
    return ok;
}
```

Keep `hvac_core.c` free of MQTT/HA concepts — it is the protocol-agnostic layer.
Publishing stays in `main.c`.

## 5. Home Assistant side — outside this repo

The combined FCU/home-cooling entity is defined in the user's HA YAML, not here.
After the firmware change, it should read from the `system_power/state` topic
(now derived) rather than templating off the raw run-status point.

**Do not attempt this edit.** Flag it as a manual follow-up for Piers and stop.

## 6. Build and verify

Source ESP-IDF 5.3.1 first.

```bash
cd firmware/bacnet_bridge && idf.py -B build-t-eth -DSDKCONFIG=build-t-eth/sdkconfig -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.t_eth_lite.defaults' build
```

Then, before any release:

```bash
tools/validate_build_profiles.sh
```

**A clean build is not evidence this works.** Per `CLAUDE.md`, a hardware change
needs live evidence naming the flashed image and the observed result. Required
live checks, on hardware:

1. All rooms on → flip System Power off in HA → every room switch goes off, HA
   switch reads `off`, FCU stops.
2. Flip one room back on → that room and only that room comes on.
3. Flip System Power **on** → no rooms turn on by themselves. (Guards the
   OFF-only decision.)
4. `manage.html` "FCU run status (readback)" still shows the raw BV:1 value,
   whatever it says.

Only one bridge may sit on the isolated `10.0.3.x` BACnet segment at a time —
both profiles use the same static address.

## 7. Scope guard

Do **not**, in this change:

- rename or repurpose BV:1 / `SYS_POWER_READBACK_INSTANCE`
- remove the System Power switch from HA discovery
- touch `bacnet-object-catalog.json` (user-generated commissioning output)
- hardcode a room count anywhere
- cascade on `on`
- refactor the surrounding MQTT publish code

Two branches, one behaviour change each. Nothing else.
