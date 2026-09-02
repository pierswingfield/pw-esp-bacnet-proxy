# LILYGO T-ETH-Lite ESP32 hardware proof

This is an isolated ESP-IDF application for the original, non-S2/non-S3
T-ETH-Lite with ESP32-WROVER-E and RTL8201. It does not contain the production
BACnet firmware and cannot control the HVAC system.

It checks:

- chip, 16 MB flash and 8 MB physical PSRAM detection;
- a 1 MiB PSRAM pattern test;
- GPIO12 PHY power enable;
- RTL8201 initialization at address 0;
- RMII external clock input on GPIO0;
- Ethernet link and ping to `10.0.3.16` from `10.0.3.99/16`;
- concurrent Wi-Fi scanning; and
- internal and PSRAM heap telemetry every ten seconds.

Read `../../docs/T_ETH_LITE_V2_ARCHITECTURE_AND_BUILD.md` before applying
power or connecting the programmer.

## Build

```bash
cd firmware/t_eth_lite_hardware_proof
source ~/esp/esp-idf/export.sh
idf.py set-target esp32
idf.py build
```

## Find the serial port

Connect only the USB downloader first, then run:

```bash
python3 ../../scripts/serial_tool.py list
```

## Manual bootloader entry

With downloader GND/TX/RX connected and the target powered from its separate
5 V pigtail:

1. Hold target BOOT.
2. Press and release target RESET.
3. Release target BOOT.
4. Flash using the detected `/dev/cu.*` port:

```bash
idf.py -p /dev/cu.REPLACE_ME flash
```

5. Press and release target RESET after the flash completes.

Do not leave downloader DTR/IO0 connected. GPIO0 is also the Ethernet reference
clock input.

## Read the serial result safely

```bash
python3 ../../scripts/serial_tool.py read \
  --port /dev/cu.REPLACE_ME \
  --baud 115200 \
  --timeout 15
```

Expected positive evidence includes:

```text
flash: 16777216 bytes (16 MiB)
PSRAM: physical=8388608 bytes (8 MiB)
PSRAM 1 MiB pattern test: PASS
PHY rail enabled: GPIO12=HIGH
Ethernet driver STARTED
Ethernet link UP
Wi-Fi scan found ... APs
```

The target ping can legitimately time out if the Delta controller is not
connected. Ethernet link-up and Wi-Fi scan remain useful independent checks.

The classic ESP32 normally maps only part of an 8 MB PSRAM chip into the normal
heap address space. The firmware prints physical and heap-mapped sizes
separately; this is not automatically a fault.

The yellow Ethernet LED may remain off at PHY address 0. Judge link state from
the serial output and actual traffic.

## Stage 0 repeatability gate

After one successful link test, flash the current proof build again so its
repeatability counter and corrected GPIO12 readback are present. A successful
cycle prints:

```text
PROOF_RESULT=PASS reset=... persistent_passes=N persistent_failures=0
```

The target ping is deliberately excluded from this result because the Delta
controller may not be connected. Flash/PSRAM identity, the PSRAM pattern test,
GPIO12 high, Ethernet link and concurrent Wi-Fi scan must all succeed.

Perform and record these as two distinct manual sets:

1. 20 warm resets using the board RESET button. Wait for `PROOF_RESULT` before
   pressing RESET again.
2. 20 cold boots by disconnecting target 5 V, waiting five seconds, restoring
   power and waiting for `PROOF_RESULT` before the next cycle.

Keep the USB-UART connected throughout, but continue to leave its IO0, RST,
3V3 and 5V pins disconnected. Classic ESP32 reset-reason reporting cannot
reliably distinguish the RESET button from power-on, so the operator must keep
the two sets separate; the persistent counter is an independent total.
