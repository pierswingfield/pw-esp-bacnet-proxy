# T-ETH-Lite ESP32 V2: hardware build and firmware architecture

Status: verified T-ETH-Lite candidate and primary-profile promotion record
Target branch: `codex/t-eth-lite-hardware-proof`
Target board: LILYGO T-ETH-Lite ESP32, ESP32-WROVER-E, RTL8201
Explicitly not the ESP32-S2/S3 T-ETH-Lite variants

## 1. What was previously recorded

There was no complete proposal for a replacement hardware platform in this
repository or its visible git history before this document.

The existing documents record three related fragments:

- the production ESP32 plus SPI W5500 dual-interface architecture;
- the August 2026 hardware investigation, including physical separation,
  power filtering and external-antenna suggestions; and
- a future HomeKit, Matter and Alexa software roadmap.

None specified the T-ETH-Lite, RTL8201, WROVER-E, its programming connection,
or a migration and parity plan. Comments in `main.c` refer to
`requirements.md` and `architecture-plan.md`, but those files are not present
in the current tree or the initial public commit.

## 2. Confirmed target hardware

The official LILYGO sources identify the original T-ETH-Lite ESP32 as:

| Property | Confirmed value |
|---|---|
| Module | ESP32-WROVER-E, classic ESP32 target |
| Flash | 16 MB |
| PSRAM | 8 MB QSPI |
| Ethernet PHY | RTL8201 over the ESP32 internal RMII EMAC |
| PHY address | 0 |
| RMII clock | External 50 MHz input on GPIO0 |
| MDC / MDIO | GPIO23 / GPIO18 |
| PHY power enable | GPIO12, active high |
| PHY reset | Board RC reset; driver reset GPIO is `-1` |
| Wi-Fi antenna | Module PCB antenna, no antenna socket |
| PoE | Optional T-ETH-Lite PoE Shield; not fitted in this build |

The RMII data pins are fixed by the classic ESP32 EMAC peripheral:

| RMII signal | GPIO |
|---|---:|
| TX_EN | 21 |
| TXD0 | 19 |
| TXD1 | 22 |
| RXD0 | 25 |
| RXD1 | 26 |
| CRS_DV | 27 |
| REF_CLK input / BOOT | 0 |

Primary sources:

- <https://github.com/Xinyuan-LilyGO/LilyGO-T-ETH-Series>
- <https://github.com/Xinyuan-LilyGO/LilyGO-T-ETH-Series/blob/master/schematic/T-ETH-Lite-ESP32.pdf>
- <https://github.com/Xinyuan-LilyGO/LilyGO-T-ETH-Series/blob/master/examples/UnitTestExample/utilities.h>

The module marking on the purchased board is authoritative if the schematic's
older WROVER-B library symbol differs from the fitted WROVER-E module.

## 3. Finished physical assembly

The new assembly is one main PCB. It does not need the production unit's
external W5500 module or any SPI Ethernet wiring.

```text
Delta DAC-1180E RJ45 ---------------- T-ETH-Lite RJ45

Regulated USB 5 V ------------------- board pad/header labelled 5V IN or VBUS
USB ground -------------------------- board GND

Temporary programming connection:
USB-UART RX ------------------------- board TXD
USB-UART TX ------------------------- board RXD
USB-UART GND ------------------------ board GND
```

### 3.1 Permanent USB power pigtail

Use a good regulated USB supply rated for at least 5 V, 2 A. The board will not
normally draw 2 A; the extra capacity avoids repeating the old marginal-power
investigation.

For a conventional four-core USB cable, red is normally +5 V and black is
normally ground. Do not trust colours alone:

1. With the cable disconnected from the board, use continuity mode to identify
   the plug's ground/shield and the intended conductors.
2. Power the cut cable and measure its polarity and voltage with a multimeter.
3. Disconnect it from power before soldering.
4. Solder +5 V only to the board pad labelled `5V IN` or `VBUS`, and ground to
   `GND`. Do not use `3V3`.
5. Cut the unused USB data conductors to different lengths and insulate each one.
6. Add heat-shrink and mechanical strain relief so cable movement cannot load
   the solder joints.
7. Inspect for solder bridges before applying power.

Use only one 5 V source at a time. When the permanent pigtail powers the target,
do not also connect a programmer's 5 V output.

### 3.2 Enclosure and RF layout

- Use nonconductive standoffs and an enclosure that cannot short the PCB.
- Leave airflow around the regulators and PHY.
- Keep the WROVER antenna end clear of metal, mains wiring and cable bundles.
- Route the Ethernet and power cables away from the PCB antenna rather than
  folding them across it.
- Retain access to TXD, RXD, GND, BOOT and RESET until OTA is proven.

The integrated PHY should remove much of the previous flying-wire/W5500
integration risk. It does not prove that Wi-Fi RF performance is fixed; this
board still has only a PCB antenna.

## 4. Using the INTER-POE downloader

The six-pin INTER-POE downloader does not mechanically plug into the T-ETH-Lite.
It belongs to the older T-Internet-POE programming arrangement. Its official
schematic shows the six signals as 3V3, GND, RST/EN, IO0, RXD and TXD.

It can still be used as a USB-to-UART adapter with individual Dupont leads.
For the safest first flash connect only:

| INTER-POE label | T-ETH-Lite label |
|---|---|
| GND | GND |
| TXD | RXD |
| RXD | TXD |

Power the T-ETH-Lite from its separate 5 V pigtail. Do not try to power the
complete T-ETH-Lite from the downloader's 3V3 pin.

Enter the ROM bootloader manually:

1. Hold the T-ETH-Lite `BOOT` button.
2. Press and release its `RESET` button.
3. Release `BOOT`.
4. Run the flash command.
5. After flashing, press and release `RESET` to boot normally.

GPIO0 is both BOOT and the Ethernet 50 MHz clock input. Do not leave a DTR/IO0
lead connected in normal operation. LILYGO warns that doing so can prevent the
PHY from initializing.

The PHY address selection changes the Ethernet LED behaviour. LILYGO says the
yellow LINK LED may remain off at address 0, so software link state and traffic
tests are authoritative.

## 5. Borrowing the production ESP32's onboard USB-UART

This is possible on most ESP32 development boards, but the INTER-POE is the
cleaner first choice because borrowing the production board takes the working
BACnet unit offline.

To use a donor ESP32 development board:

1. Disconnect power from both assemblies before wiring.
2. Hold the donor ESP32 itself in reset by wiring donor `EN` to donor `GND`.
   This keeps its UART0 pins from competing with the target.
3. Connect donor `TX0` to T-ETH-Lite `RXD`.
4. Connect donor `RX0` to T-ETH-Lite `TXD`.
5. Connect donor `GND` to T-ETH-Lite `GND`.
6. Power the T-ETH-Lite from its own 5 V pigtail. Do not join the boards' 5 V
   rails.
7. Put the T-ETH-Lite into download mode using its own BOOT/RESET buttons.
8. Flash through the serial port created by the donor board's USB connection.
9. Manually reset the T-ETH-Lite after flashing.

This relies on the donor board exposing UART0 and EN in the normal development
board arrangement. Verify its pin labels before connecting anything. Never use
the production device's live in-wall wiring as a convenient ground or power
source.

## 6. Is anything else required?

Given the purchased boards and existing tools, no additional active electronic
module is required for initial flashing.

Available and sufficient:

- T-ETH-Lite main board;
- INTER-POE downloader used through Dupont leads;
- Dupont leads;
- soldering equipment;
- Ethernet patch lead; and
- a verified USB 5 V pigtail plus suitable power supply.

Strongly recommended for a beginner:

- digital multimeter for polarity, continuity and short checks;
- heat-shrink tubing;
- strain relief or a cable gland;
- nonconductive enclosure and standoffs; and
- labels on both ends of the temporary UART leads.

A generic CP2102/CP2104/CH340 USB-UART adapter would be inexpensive and more
convenient, but it is optional rather than a blocker.

## 7. Firmware migration architecture

The production `master` branch remains the recovery line for the W5500 unit.
The new hardware is developed on `codex/t-eth-lite-hardware-proof`.

### Stage 0: hardware proof

The standalone app under `firmware/t_eth_lite_hardware_proof` must establish:

- chip, flash and PSRAM detection;
- destructive pattern testing of a temporary PSRAM allocation;
- GPIO12 PHY power control;
- RTL8201 initialization at address 0;
- static Ethernet address `10.0.3.99/16`;
- link-up/down recovery and ping to `10.0.3.16`;
- concurrent Wi-Fi scanning with Ethernet energized; and
- internal/PSRAM heap telemetry.

Gate for the initial board: 10 cold boots and 20 warm resets with reliable
Ethernet initialization. The cold-boot sample was reduced from 20 to 10 by the
owner after the first live batch.

### Stage 1: minimum-change production port

Add a board profile to the production firmware:

```text
Internal ESP32 EMAC: enabled
SPI Ethernet: disabled
PHY: RTL8201
PHY address: 0
MDC / MDIO: GPIO23 / GPIO18
PHY power: GPIO12 high
PHY reset: -1
RMII clock: external input on GPIO0
Target: esp32
Flash: 16 MB
PSRAM: enabled
```

The current BACnet datalink already binds to a supplied `esp_netif`, so it is
not intrinsically W5500-specific. Preserve the static isolated network, NVS
keys, REST contracts, MQTT topics and Home Assistant entity IDs.

Gate: full functional parity before structural changes.

### Stage 2: flash and memory

- Use two large OTA slots and a larger persistent coredump partition.
- Give the bootloader more growth space than the current 0x730-byte margin.
- Use PSRAM for logs, scan pages, catalog data and large JSON buffers.
- Initially keep task stacks, EMAC DMA data and synchronization objects in
  internal DRAM. Stage 1 measurements showed that keeping every Wi-Fi/LwIP
  buffer internal as well did not leave enough contiguous memory for the
  measured HTTP and MQTT task stacks, so the current T-ETH-only candidate also
  enables ESP-IDF's supported Wi-Fi/LwIP-in-PSRAM policy.
- Retain allocation limits, task headroom and largest-free-block telemetry.

### Stage 3: component split

| Component | Exclusive responsibility |
|---|---|
| `board_support` | Board profile, pins, PHY power and hardware capabilities |
| `connectivity` | Ethernet, Wi-Fi STA/AP and interface state |
| `config_store` | Versioned NVS schema, import/export and migration |
| `bacnet_service` | BACnet socket, binding, transactions, discovery and cache |
| `hvac_domain` | Rooms, setpoints, power and boost lifecycle |
| `mqtt_bridge` | MQTT recovery, HA discovery, commands and state |
| `web_api` | HTTP routing and presentation only |
| `ota_service` | Authentication, streaming, validation and rollback |
| `diagnostics` | Logs, task registry, heap/PSRAM metrics and coredumps |
| `app_main` | Initialization order and component supervision |

Replace direct BACnet calls from HTTP and MQTT tasks with one long-lived BACnet
worker that exclusively owns the stack and socket:

```text
HTTP / MQTT / scanner -> bounded request queue -> BACnet worker
                                            -> bounded response pool
```

This removes cross-task BACnet access, reduces HTTP/MQTT stack requirements and
makes queue pressure and timeouts observable. Use fixed request objects and no
unbounded per-request allocation.

### Stage 4: stability parity

The following are mandatory behaviours:

- checked task creation and task registry;
- static BACnet task stack;
- total and largest internal heap telemetry;
- measured task-stack high-water marks;
- Ethernet failure degrades to Wi-Fi-only;
- Wi-Fi exponential backoff, jitter, self-rearming retry and reboot backstop;
- bounded MQTT outbox and client recycling;
- precise BACnet cache invalidation after writes;
- paged object scanning and idle catalog release;
- opt-in mDNS;
- authenticated dual-slot OTA rollback; and
- persistent coredumps with matching ELF retention.

Do not mark a new OTA image valid at the first line of `app_main`. Mark it only
after core initialization, critical task creation and an internal health
window. External BACnet and MQTT availability must not be required for the
health decision.

### Stage 5: functional parity matrix

Verify provisioning, saved Wi-Fi, isolated Ethernet, BACnet bind/read/write,
unicast discovery, room mapping, system/room power, setpoint readback, boost,
object scanning, custom MQTT points, HA discovery/commands, all REST endpoints,
configuration import/export, factory reset, OTA and diagnostics.

### Stage 6: release acceptance

- 20 cold and 20 warm boots.
- Ethernet and Wi-Fi disconnect/recovery tests.
- One-hour broker outage.
- Full object scan during MQTT and dashboard activity.
- Good and deliberately failing OTA rollback tests.
- 24-hour development and 72-hour release-candidate soaks.
- Zero unexpected resets, panics or coredumps.
- No downward heap trend.
- Largest internal block exceeds the largest measured transient allocation by
  at least 25 percent.
- Repeat the old `/api/status` RF dose-response test with Ethernet linked.
- No recurrence of the approximately 30 dB Wi-Fi uplink asymmetry.

## 8. Known baseline

Before this branch was created, `master` at `2a78c0d` built with ESP-IDF 5.3.1:

```text
bacnet_bridge.bin size: 0x163e60
smallest app partition: 0x1db000
app partition free: 0x771a0 (25 percent)
bootloader size: 0x68d0
bootloader free before partition table: 0x730 (6 percent)
```

This is a mechanical baseline only. Production behaviour remains dependent on
live device checks and soak testing.

## 9. Live Stage 0 evidence

On 2026-09-02 the first powered hardware proof established:

- the RTL8201 Ethernet link remained UP throughout a 50-second capture;
- Wi-Fi scanned successfully while Ethernet remained linked;
- internal and PSRAM free/largest-block telemetry remained stable during the
  capture; and
- ping to `10.0.3.16` timed out, which is not a hardware failure when the Delta
  controller is absent or not reachable on the attached layer-2 network.

This is one successful functional observation, not completion of the Stage 0
reliability gate. The proof firmware now emits a persistent pass/failure count.
The operator reported the warm-reset batch complete. Codex directly monitored
10 consecutive five-second target-power-off cycles on 2026-09-02: persistent
passes advanced from 10 through 20, and every cycle showed POWERON reset,
8 MiB PSRAM detection and pattern-test pass, GPIO12 high, Ethernet link UP and
a concurrent Wi-Fi scan. The stored four earlier failures were deliberate
no-Ethernet test runs and were excluded from this connected-cable batch.

The accepted initial-board gate is therefore 20 RESET-button cycles plus these
10 cold boots. Repeat the larger release-acceptance boot set before deployment.

### Stage 1 build profile

The primary application build now selects the T-ETH-Lite profile by default.
The common defaults remain in `sdkconfig.defaults`; board-specific defaults
are selected by the project CMake configuration:

```bash
idf.py -B build-t-eth-lite \
  -DSDKCONFIG=/tmp/esp-bacnet-t-eth-lite-sdkconfig \
  build
```

The T-ETH-Lite profile enables RTL8201 address 0, MDC GPIO23, MDIO GPIO18,
external RMII clock input on GPIO0, PHY power on GPIO12 and 16 MiB flash. Its
current memory policy exposes PSRAM to ordinary allocation, sends allocations
of at least 16 KiB there when possible, permits ESP-IDF-managed Wi-Fi/LwIP
buffers in PSRAM, and reserves 32 KiB of internal memory for DMA/internal-only
requests. Task stacks, EMAC DMA allocations and synchronization objects remain
internal.

Do not flash this production image as if it were another inert proof image. It
contains the BACnet, MQTT, web-control and OTA application. Its automatic
BACnet startup test reads the target device name and a room setpoint; control
writes remain driven by authenticated/user or MQTT commands. Perform the first
production-port boot with the serial console attached and no unattended MQTT
command source.

After entering the ROM bootloader manually, flash the Stage 1 image with:

```bash
cd firmware/bacnet_bridge
idf.py -B build-t-eth-lite \
  -DSDKCONFIG=/tmp/esp-bacnet-t-eth-lite-sdkconfig \
  -p /dev/cu.REPLACE_ME flash
```

For the retained W5500 recovery profile, explicitly select
`-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.w5500.defaults"` and use
a separate build and sdkconfig directory. Do not flash that 4 MiB profile to
the 16 MiB T-ETH-Lite.

Stage 1 live acceptance requires, in order:

1. normal SPI-flash boot with 8 MiB physical PSRAM detected;
2. `PHY power enabled on GPIO12` and Ethernet link UP;
3. static isolated Ethernet address and successful ping to the Delta target;
4. BACnet bind plus device-name and room-setpoint reads;
5. saved-Wi-Fi or provisioning flow and dashboard reachability;
6. no reset, panic, brownout or internal-heap regression during observation;
7. one deliberately disconnected-Ethernet boot that remains reachable over
   Wi-Fi rather than boot-looping; and
8. only after those read-only checks, controlled BACnet write/readback tests.

### Stage 1 first flash evidence

On 2026-09-02 the production-profile image was flashed to the T-ETH-Lite. Two
normal stub/compressed attempts failed with UART data-checksum errors over the
temporary Dupont connection; the second attempt had already erased part of the
bootloader region. Recovery used the ESP32's immutable ROM loader at 115200
baud with no stub and no compression. All four written regions reported
`Hash of data verified`: bootloader, 1,513,472-byte application, partition
table and initial OTA metadata.

The subsequent 120-second serial capture confirmed:

- normal `SPI_FAST_FLASH_BOOT` from OTA slot 0;
- 16 MiB flash and 8 MiB physical PSRAM, with the startup memory test OK;
- coredump partition discovery;
- GPIO12 PHY power enable;
- RTL8201 netif creation and static address `10.0.3.99`;
- provisioning AP `ESP-BACnet-Setup` at `192.168.4.1`; and
- no saved Wi-Fi credentials, as expected on this new board.

Ethernet was intentionally disconnected, so the BACnet task correctly remained
at `Waiting for Ethernet link...`. The provisioning log also warned that the
OTA password was unset at that point.

The subsequent Wi-Fi wizard run connected the board to `PieFi`, assigned
`192.168.1.3`, saved the OTA password and rebooted normally. A live
`/api/network` check then reported `ota_password_set: true`,
`mdns_enabled: false`, no Ethernet link and a software-restart reset reason.
No password or other secret is recorded in this document.

The same live session exposed a Stage 1 memory-headroom gate before connection
to the Delta network. `/api/debug/stacks` reported 18,668 bytes of free internal
heap, a 14,848-byte largest free block and a 3,964-byte minimum, even with
Ethernet disconnected and no MQTT broker configured. The unconfigured MQTT
startup path still reserves a 24,576-byte `mqtt_command` task stack and its
queue. The corrective candidate defers those resources until a broker is saved
and changes only the T-ETH-Lite profile to route ordinary allocations larger
than 16 KiB to PSRAM. Task stacks, DMA allocations, Wi-Fi and LwIP buffers remain
in internal RAM.

The rebuilt image was flashed and independently verified byte-for-byte on
2026-09-02. Its captured boot passed the 8 MiB PSRAM test, reserved 32 KiB for
DMA/internal-only allocations, restored the saved Wi-Fi and OTA-password state,
and logged that unused MQTT command resources were deferred. Ten consecutive
`/api/status` requests left the ordinary internal heap at 17,475 bytes with a
12,239-byte minimum, compared with the preceding image's 3,964-byte minimum.
The existing diagnostic deliberately requests both `MALLOC_CAP_8BIT` and
`MALLOC_CAP_INTERNAL`, so it does not include the separate 32 KiB pool whose
capabilities are reserved for DMA/internal-only requests. At 97 seconds uptime
the reset reason remained `Power-on`, with no panic or restart. This closes the
Wi-Fi-only memory gate.

For wired Stage 1 acceptance, the production bridge was first disconnected to
avoid two devices using `10.0.3.99`. Its direct Delta Ethernet cable was moved to
the T-ETH-Lite. The new board reported Ethernet linked, identified controller
`C305` at `10.0.3.16`, and returned valid system-power, boost, temperature,
setpoint and room-power values throughout seven repeated API samples. A no-op
write of Room A's existing 18.0 C setpoint returned
`{"ok":true,"setpoint":18.0}` and read back as 18.0 C. At 430 seconds uptime,
the reset reason remained `Power-on`, the ordinary internal-heap minimum was
12,115 bytes, and there had been no panic or link loss. This proves basic
Ethernet, Delta read and Delta write operation, but a later 483-second snapshot
showed that the HTTP task's worst-case stack headroom had fallen to only 1,108
bytes. The succeeding T-ETH-only profile raised the connected-mode HTTP stack
to 32,768 bytes and enabled ESP-IDF's Wi-Fi/LwIP-in-PSRAM allocation policy,
while retaining task stacks and a 32 KiB DMA/internal pool inside the chip.
The verified current-result record in Section 10 shows that all HTTP and MQTT
tasks now start and retain positive headroom under connected traffic. If a
future measured workload again approaches stack exhaustion, use the planned
single BACnet-worker queue rather than continuing to enlarge competing task
stacks.

### Backlog captured during Stage 1 setup

- The post-Wi-Fi `Saved` response in `main.c` always tells the user to open
  `http://esp-bacnet-bridge.local`, even when the provisioning-screen mDNS
  toggle was left disabled. Generate this completion message from the saved
  mDNS state: show the `.local` address only when advertising is enabled;
  otherwise state that mDNS is disabled and tell the user to find the assigned
  IP address in the router's connected-device/DHCP-client list. Retain the
  router-IP fallback when mDNS is enabled but unsupported by the client.

## 10. Verified T-ETH-Lite integration state (2026-09-02)

This is the operational record for the current candidate. Earlier stages
remain the design and historical evidence; they are not instructions to
rebuild or reflash this already-tested image.

### Current physical and live state

- The T-ETH-Lite is powered, booted after a full power cycle and reachable at
  `192.168.1.3` over saved Wi-Fi. MQTT is connected; credentials remain out
  of this repository.
- The fresh T-ETH image used a generated sdkconfig with
  `CONFIG_SPIRAM_USE_MALLOC=y`,
  `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` and
  `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`.
- The application image was `0x171d00` (1,514,752 bytes), SHA-256
  `52c5c03e96d493addea9eadf9d8354f6bb0d66cb4fff2e2226d33cf57e947008`.
  Only the application partition at `0x20000` was written; an independent
  `esptool verify_flash` returned `verify OK (digest matched)`.
- With the W5500 production bridge off the shared segment, the T-ETH-Lite
  linked to the isolated Delta cable, discovered `C305` at `10.0.3.16`, and
  returned valid system, boost, room setpoint, temperature and power values.
- `/api/debug/stacks` reported the requested 32,768-byte HTTP task, a
  24,576-byte MQTT command task and a 16,384-byte MQTT state task, all with
  positive headroom in the connected session. Sixty connected-mode API calls
  completed without a reset or request failure.
- The UART adapter supports ROM flashing, but this session did not capture
  application-console bytes. Do not describe this entry as fresh serial proof
  of the PSRAM startup-pattern test.

### Repository state and active soak

- Branch: `codex/t-eth-lite-hardware-proof`. Principal changes are this
  architecture record, the hardware proof project, the T-ETH defaults,
  configurable PHY power, deferred MQTT resources and the T-ETH stack and
  diagnostics work.
- The 15-minute connected soak sampled `/api/status`, `/api/network`,
  `/api/mqtt/config` and `/api/debug/stacks` every 30 seconds. All 31 samples
  (124 API requests) succeeded: Ethernet and MQTT remained connected, uptime
  advanced from 475 to 1,389 seconds without a reset, free heap stayed between
  27,971 and 28,855 bytes, and HTTP/MQTT task headroom stayed positive. This
  completes the short connected-soak gate; it does not replace the longer
  development and release-duration soaks below.
- Keep only one bridge on the shared `10.0.3.x` segment at a time: the T-ETH
  and W5500 profiles use the same static address. Do not erase NVS or whole
  flash during this validation.

### Architecture decision gate

If all required tasks start and mixed HTTP/MQTT/BACnet activity retains a
comfortable measured margin, proceed to the soak matrix. Do not select a fixed
margin from intuition: record high-water marks under the worst observed load,
then apply the Stage 6 25-percent transient-allocation margin.

If either network task still cannot start, or a task again approaches stack
exhaustion, stop enlarging competing task stacks. Implement Stage 3's single
long-lived BACnet worker with a bounded queue and fixed request/response
objects. The application already allocates a static 24 KiB `BacnetTaskStack`
for an initial probe that later self-deletes; reuse that ownership model so
HTTP, MQTT and scans submit work rather than running BACnet transactions on
their own stacks. Use bounded waits/task notifications or static semaphores,
make queue pressure observable, and avoid heap allocation per request. Then
resize HTTP/MQTT stacks from measured high-water marks.

### Outstanding work before release

- Complete the full Stage 6 matrix: 20 cold and 20 warm boots (the proof's 20
  warm/10 cold observations are preliminary), Ethernet and Wi-Fi recovery,
  one-hour broker outage, full object scan during MQTT/dashboard load, good and
  deliberately failing OTA rollback, 24-hour development soak and 72-hour
  release-candidate soak.
- Require zero unexpected resets, panics or coredumps, no downward heap trend,
  and adequate largest-block margin throughout.
- Repeat the RF dose-response test with Ethernet linked.
- Complete the component split and OTA-validity/rollback changes described in
  Stages 2-4; the minimal Stage 1 port does not itself constitute the planned
  rearchitecture.
- Increase bootloader growth margin from the recorded 0x730 bytes when the
  partition/OTA work is undertaken.
- Fix the mDNS-dependent post-Wi-Fi completion text in the backlog above.
- Update all affected documentation and run both T-ETH and W5500 builds before
  committing. Do not claim feature/stability parity from a clean build alone.

Do not erase the whole flash or NVS during the next test, power the target from
both boards, connect both bridges to the Delta segment, or revert to the
unreliable high-speed/stub flashing method on the present wiring.
