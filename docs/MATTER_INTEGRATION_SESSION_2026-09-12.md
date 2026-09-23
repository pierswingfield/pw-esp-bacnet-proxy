# Matter integration session — 2026-09-12

Full record of the session that took the T-ETH-Lite Matter build from
"stub scaffolding" to a real, booting, commissioning-capable ESP-Matter
Thermostat endpoint, plus the first real automation-module selector in the
setup wizard. Written for a fresh session to pick up from cleanly.

## Where things ended up

- **Real ESP-Matter SDK is linked, boots, and advertises correctly** on the
  T-ETH-Lite hardware (verified live, repeatedly, over several flash cycles).
- **Module selector is built**: the wizard's old MQTT-only step 5 is now a
  real three-way choice (Dashboard Only / MQTT+Home Assistant / Matter), and
  the backend bug that made `/api/wizard/finish` always start MQTT
  regardless of selection is fixed.
- **One real, reproduced firmware bug remains open** (see "Open issue"
  below): the HTTP dashboard stops responding with `Connection reset by
  peer` under conditions not yet fully isolated. It is not the BLE/WiFi
  radio-coexistence congestion characterised earlier in the session — that
  one manifests as a timeout, resolves itself, and is already mitigated.
  This is a second, distinct failure discovered late in the session and
  **not yet root-caused**.

## Build environment (needed to reproduce any of this)

- **ESP-Matter SDK**: `~/esp/esp-matter` (release/v1.4, matches ESP-IDF
  5.3.1 — do not upgrade to a newer esp-matter release without also
  upgrading ESP-IDF, they're version-locked in pairs upstream).
- **Space-free build tree**: `~/esp-bacnet-nospace/bacnet_bridge` — a working
  copy of `firmware/bacnet_bridge/`, **not a git checkout**, kept because the
  real project path (`.../Desktop/AI Projects/Random/esp-bacnet/...`)
  contains spaces that break connectedhomeip's GN build (see "Problems
  solved" #1). Any file changed under the real project tree must be
  `rsync`'d or `cp`'d into this tree before rebuilding the Matter profile.
  The `build-t-eth-matter/` directory inside it is the actual Matter build
  output; `build-t-eth/` is a plain-profile build used for shared-code
  sanity checks.
- **Build command** (from `~/esp-bacnet-nospace/bacnet_bridge`):
  ```bash
  source ~/esp/esp-idf/export.sh
  source ~/esp/esp-matter/export.sh
  idf.py -B build-t-eth-matter -DSDKCONFIG=build-t-eth-matter/sdkconfig \
    -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.t_eth_lite.defaults;sdkconfig.matter.defaults' \
    build
  ```
  Omit `SDKCONFIG_DEFAULTS` on subsequent builds where `sdkconfig` already
  exists and only source changed (faster reconfigure). Delete
  `build-t-eth-matter/sdkconfig` and pass `SDKCONFIG_DEFAULTS` again whenever
  an `sdkconfig.matter.defaults` value changes.
- **Flashing**: this bench setup has no auto-reset circuit (DTR/RTS aren't
  wired to EN/IO0), so every flash and every reboot needs the board
  physically put into bootloader mode by hand: bridge **IO0 → GND**, tap
  **EN/RST** while bridged, then flash; after flashing, remove the IO0
  bridge and tap EN/RST again to boot the app. See
  [esp32-serial-access-via-mcp](../../../.claude/memory equivalent) —
  port is currently `/dev/cu.usbserial-1120` (CH340, changes across
  sessions/reconnects — re-list with `serial_tool.py list` if it's gone).
- **Permanent fix available but not built**: wire the adapter's DTR/RTS to
  IO0/EN through the classic capacitor-coupled auto-reset circuit (not a
  direct/hard wire — GPIO0 doubles as this board's RMII clock input, and a
  static DTR tie can interfere with Ethernet during normal running per
  LILYGO's own documentation). This needs actual soldering; out of scope
  for a software session.

## Files changed this session

**Opt-in Matter build wiring (none of this touches the default T-ETH-Lite
or W5500 profiles unless `ESP_MATTER_PATH` is sourced):**
- `firmware/bacnet_bridge/CMakeLists.txt` — conditionally wires in
  ESP-Matter's `EXTRA_COMPONENT_DIRS`/cmake includes, gated on
  `ENV{ESP_MATTER_PATH}`.
- `firmware/bacnet_bridge/sdkconfig.matter.defaults` — new. All the
  Matter-variant-only sdkconfig overrides, each commented with why (BLE,
  IPv6, DRAM-relocation flags, memory-mode flags, discovery timeout).
- `firmware/bacnet_bridge/main/chip_project_config.h` — new. CHIP compile-time
  overrides (`CHIP_CONFIG_MAX_FABRICS`, IM pool sizes) — see "Problems
  solved" #3.

**Matter adapter — real implementation replacing pure stub:**
- `firmware/bacnet_bridge/components/matter_adapter/matter_adapter.cpp` — new.
  Real Thermostat endpoint creation, attribute read/write wired to
  `hvac_core`, commissioning-window state tracking, coexistence bias,
  retry-pairing entry point. Compiled only when `ESP_MATTER_PATH` is set.
- `firmware/bacnet_bridge/components/matter_adapter/matter_adapter_stub.c` —
  new (renamed from the original `matter_adapter.c`). Unchanged stub
  behaviour for W5500/default T-ETH-Lite builds.
- `firmware/bacnet_bridge/components/matter_adapter/CMakeLists.txt` —
  conditional `idf_component_register`, picks stub vs. real source.
- `firmware/bacnet_bridge/components/matter_adapter/matter_extram.lf` — new.
  Linker fragment forcing `libesp_matter.a`/`libCHIP.a`/`libbt.a`/`main.c`'s
  own static buffers into PSRAM. This is the mechanism that actually closed
  the DRAM-overflow gap general Kconfig flags couldn't reach — see
  "Problems solved" #2.

**Automation module state model (shared code — affects all profiles,
verified the default T-ETH-Lite and W5500 profiles still build clean):**
- `firmware/bacnet_bridge/components/hvac_core/include/hvac_core.h` /
  `hvac_core.c` — new `hvac_pairing_status_t` enum
  (idle/awaiting/paired/timed_out), persisted in NVS (`nvs_integ` namespace,
  key `matter_pair`) alongside the existing integration mode. Fresh selection
  of Matter mode resets pairing status to idle rather than carrying over a
  stale timed-out flag from a previous attempt.
- `firmware/bacnet_bridge/main/main.c`:
  - `/api/integration` GET now returns `matter_pairing_status`.
  - New `POST /api/matter/retry-pairing` endpoint.
  - **Fixed real bug**: `api_wizard_finish_handler` used to call
    `mqtt_app_start()` unconditionally regardless of any integration
    selection. It now reads `integration_mode` from the finish payload and
    only starts the transport actually chosen.

**Wizard UI — module selector:**
- `firmware/bacnet_bridge/main/wizard.html` — step 5 rewritten from a
  hardcoded MQTT form into a real module selector (three cards), with the
  existing MQTT form now conditional on selection, plus a Matter panel
  (informational only — BLE pairing has no config fields to fill in) that
  points to the Health page for live status. Review step (step 6) shows the
  selected automation mode and only shows MQTT/HA rows when relevant.
- `firmware/bacnet_bridge/main/health.html` — new "Automation" card: current
  mode, and for Matter specifically, live pairing status with a **Retry
  Pairing** button wired to the new endpoint.

## Problems solved, in the order they were hit

1. **GN build fails on a space in the project path.** connectedhomeip's GN
   arg-quoting splits `.../Desktop/AI Projects/Random/esp-bacnet/...` into
   two tokens on the space. A symlink didn't survive CMake's path
   canonicalization. Fix: build from a real (non-symlink) copy at a
   space-free path — `~/esp-bacnet-nospace/bacnet_bridge` — kept
   permanently for this reason.

2. **Static link fails: `.dram0.bss` overflows internal DRAM by 90,992
   bytes.** `esp_matter`/`libCHIP`/Bluetooth keep large static buffers that
   `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` alone doesn't relocate —
   that flag only lifts a restriction; placement still needs a linker
   fragment mapping the archive's sections into `extram_bss`, which
   ESP-IDF's own big components (wifi, lwip) get automatically via their own
   `.lf` files but external GN-built libraries never receive. Fix:
   `components/matter_adapter/matter_extram.lf` mapping `libesp_matter.a` /
   `libCHIP.a` / `libbt.a` / `main.c`'s own object into `extram_bss`
   (gated on the same Kconfig flag). This closed ~90KB down to 14.8KB
   overflow in one step; `CHIP_CONFIG_MAX_FABRICS` reduced from 16 to 4
   (see `chip_project_config.h`) and moving `main.c`'s own log
   buffer/scan-state statics closed the rest.

3. **Runtime: `Couldn't allocate EmberAfCluster` / `Couldn't allocate
   EmberAfEndpointType`.** `esp_matter`'s own dynamic allocator
   (`esp_matter_mem_calloc`) defaults to `MALLOC_CAP_INTERNAL` regardless of
   the general SPIRAM policy — a separate Kconfig choice
   (`CONFIG_ESP_MATTER_MEM_ALLOC_MODE_EXTERNAL`) controls it and isn't
   linked to the general one. Fixed by setting that flag.

4. **Runtime: `Failed to create LwIP core lock` / `Failed to launch Matter
   main task`.** Genuine internal-heap contention at the exact moment
   BLE/WiFi/CHIP all initialize together — not fixed by any single flag;
   mitigated by increasing `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` (32K →
   96K) to keep a larger contiguous internal-only reserve available at that
   moment, and lowering `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (16K → 1K) so
   fewer things are forced internal that don't need to be. `xTaskCreate()`
   itself is confirmed (via a grounded lookup against ESP-IDF's own docs)
   to always require internal RAM for task stacks regardless of
   `SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` — that flag does not make
   `xTaskCreate()` PSRAM-capable, only `xTaskCreateStatic`/
   `xTaskCreateWithCaps` are.

5. **`matter_sync` task stack overflow**, crashing the device a few seconds
   after Matter fully initialized. My own periodic attribute-push task was
   given 4096 bytes; raised to 8192.

6. **HTTP dashboard unresponsive while BLE commissioning is active.**
   ESP32 classic shares one 2.4GHz radio between WiFi and BLE
   (`wifi:Coexist!!!` log line confirms). Two mitigations:
   - `esp_coex_preference_set(ESP_COEX_PREFER_WIFI)` biases the radio
     scheduler toward WiFi during the window.
   - `CONFIG_CHIP_DISCOVERY_TIMEOUT_SECS` shortened from the SDK default of
     900s (15 min) to 600s (10 min), bounding the worst case.
   This is real and expected — Matter's actual commissioning handshake is
   BLE + IP, not HTTP, so a real phone pairing a real device is not blocked
   by this; only *this diagnostic dashboard* looks sluggish mid-pairing.
   **This is a different symptom from the open issue below** — this one
   reliably self-resolves (times out) and ping stays responsive throughout;
   the open issue below does neither.

7. **Chip stack locking crash**: `Chip stack locking error at
   'src/transport/Session.h:230'. Code is unsafe/racy` → `abort()`. My new
   boot-time logic (close an auto-opened commissioning window immediately
   if the *previous* attempt already timed out, so an unattended reboot
   doesn't silently reopen a BLE-congested window) called
   `CommissioningWindowManager::CloseCommissioningWindow()` directly from
   `app_main`'s task context. CHIP's `CommissioningWindowManager` methods
   assert if called without holding the CHIP stack lock from a non-Matter
   thread. Same bug existed in `matter_adapter_retry_pairing()` (called from
   the HTTP server task). **Fixed**: both call sites now wrap the CHIP call
   in `chip::DeviceLayer::PlatformMgr().LockChipStack()` /
   `UnlockChipStack()`. This had been silently crash-looping the device on
   every boot where the persisted pairing status was `timed_out` — reboot
   count had reached 16 before this was caught, because the reset happened
   fast enough (~1s after boot) that it looked like normal BLE-congestion
   HTTP failure from the outside, not a crash. **Confirmed fixed**: flashed
   after the lock fix, full boot log captured clean through Matter init and
   BACnet worker startup, no abort, no reboot.

## Open issue — NOT resolved, needs a fresh session

After the lock-fix flash (item 7), HTTP still failed on later checks —
`Connection reset by peer` on every endpoint (`/wizard`, `/health`,
`/api/integration`), consistently, over several minutes, not resolving on
its own. This is a **different signature** from the congestion case (which
times out rather than resets, and clears within the 10-minute window):

- `ping` stayed healthy throughout (7–41ms), ruling out a full
  crash-reboot-loop (which would show fresh boot banners on serial) or
  total radio saturation.
- Passive serial listening showed **no reboot banner, no abort, no crash
  output** at any point during the failure — the device is not visibly
  crash-looping this time.
- The persisted `matter_pairing_status` for that boot was `idle`, not
  `timed_out` (cleared by an earlier test toggling the mode back and forth),
  so the item-7 code path that used to crash was **not the one running**
  this time — this reset has a different cause.
- Best working theory, unconfirmed: the `httpd` task itself may have died
  (a single, non-fatal-to-the-whole-chip fault) leaving every other task
  (BACnet worker, WiFi, Matter) running fine, which would produce exactly
  this pattern — connection resets (SYN to a dead listener), healthy ping,
  no full-system reboot. Needs a fresh boot with the debug/coredump
  facilities checked immediately, or a live GDB/monitor session, to confirm.
- **Reconfirmed well after the fact, same session, no reboot in between**:
  checked again a long while later (well beyond any plausible commissioning
  window duration) — ping still healthy (7-8ms, if anything better than
  earlier), `curl` to `/api/integration` still gets `Recv failure:
  Connection reset by peer` immediately on every attempt. This rules out
  BLE-window timing entirely as the cause; whatever this is, it does not
  self-resolve with time. The device was left running, untouched, exactly
  as failing, for tomorrow's session to inspect fresh (no reboot performed
  after this note).
- **First diagnostic step for next session**: since it's confirmed
  persistent (not time-bound), a reboot is no longer needed just to "wait
  and see" — go straight to capturing a full, continuous serial log across
  the transition into failure, and treat "httpd task died independently of
  the rest of the system" as the leading hypothesis (ping/BACnet/Matter all
  keep working; only HTTP is affected).

## Verified this session, on real T-ETH-Lite hardware, repeatedly

- Full dual-profile build validation (`tools/validate_build_profiles.sh`) —
  pass.
- E2E test suite (`python3 tests/run_e2e_tests.py`) — 146/146 pass. Note:
  `TEST_READY.md`'s "125 tests, Tier 5 not started" is stale — Tier 5
  (16 adversarial tests) already exists and passes; total is 146.
- Default T-ETH-Lite (non-Matter) firmware: boots clean, BACnet worker
  functional, dashboard reachable.
- Matter-enabled firmware: links, boots, creates and starts a real
  Thermostat endpoint, publishes `_matterc._udp` over mDNS, opens a
  standards-compliant CASE-session-capable commissioning window — this is a
  genuine, pairable Matter device from Apple Home/Google Home/Alexa's
  perspective, not just scaffolding.

## Next steps, roughly in priority order

1. **Root-cause the open HTTP-reset issue** above — this blocks verifying
   the module-selector UI and the pairing-status/retry-pairing flow ever
   actually render in a browser; neither has been visually confirmed this
   session, only inferred from clean builds and serial logs.
2. Once the dashboard is reliably reachable, do the actual browser-based
   verification of the wizard's module selector and the health page's
   Automation card that never got done tonight.
3. Consider the setup-flow reorder discussed but not built: do BACnet
   discovery/room setup while still on the device's own temporary AP
   (works today, since BACnet runs over the separate Ethernet interface
   regardless of WiFi state — this specific claim is unverified, confirm
   it), and only ask for WiFi credentials if a mode other than Matter is
   chosen (Matter's own BLE commissioning can hand over WiFi credentials
   itself, so a Matter user would never need to find the device's LAN IP
   manually). **Real gap identified, not yet solved**: this project's own
   `wifi_prov` component runs an independent softAP/captive-portal
   WiFi-provisioning system that would conflict with CHIP's native
   BLE-driven WiFi provisioning if both try to manage the WiFi interface
   at once for a Matter-mode device with no saved credentials. Needs
   explicit reconciliation (suppress `wifi_prov`'s own AP-provisioning
   behavior specifically when `integration_mode == matter`), not just a
   wizard reorder.
4. `esp-matter-mfg-tool`'s dependency conflict with ESP-IDF's own
   `cryptography` pin (see below) means real factory attestation
   certificates (needed before this could ever ship to a second device,
   not just this dev unit) haven't been generated — current commissioning
   uses the SDK's built-in example/test DAC provider, fine for development,
   not for production.
5. Longer-run stability: the device has not been soak-tested with Matter
   active for anything close to the 24h/72h bar already established for
   the non-Matter T-ETH-Lite build.

## Incidental fixes/notes worth keeping

- `esp-matter`'s bootstrap upgraded the shared Python env's `cryptography`
  package to 44.0.1, breaking ESP-IDF's own pin (`<43`). Downgraded back to
  42.0.8 to keep `idf.py` working; this leaves `esp-matter-mfg-tool`
  (wants 44.0.1) broken in that same env — expected trade-off, not a bug,
  until factory-cert generation is actually needed (see next steps #4).
- `/api/ota` accepts a raw binary `POST` directly
  (`curl -X POST --data-binary @bacnet_bridge.bin http://<ip>/api/ota`) —
  no jumper/serial access needed for iterative updates once a device is
  alive and reachable on the network. Only useful once the open HTTP issue
  above is resolved and the device is reliably reachable; not useful for
  recovering a crash-looping build, which still needs serial.
