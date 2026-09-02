# Debug logging & diagnostics

How to observe this bridge when something goes wrong. Written for whoever —
human or agent — picks up a fault next.

Replace `<BRIDGE_IP>` with the bridge's address and `<PI_HOST>` with whatever
host runs the syslog listener.

---

## 1. On-device diagnostic log

`diag_log()` writes to the serial console (`ESP_LOGW("DIAG", ...)`). Every
line carries the same prefix:

```
esp-bacnet up=<seconds>s heap=<free>K big=<largest-block>K <message>
```

**`big=` is the number that predicts allocation failures, not `heap=`.** Task
stacks and buffers need *contiguous* memory. A device showing 33K free but a
6K largest block cannot create a 24K task, and total-free alone hides that.

Wired into:

- WiFi connect/disconnect, with reason code and retry count
- MQTT connect/disconnect, and the client recycle
- `system-power` / `room-power` API writes
- `/api/status` and `/api/health` entry and exit, with duration **and
  `stack_free=`** (the handler task's stack high-water mark)
- The periodic BACnet poll burst, entry and exit with duration
- Object-catalog allocation and idle release
- The WiFi link watchdog and the no-IP reboot backstop

### Reading `stack_free=`

This is `uxTaskGetStackHighWaterMark()` — the minimum free stack ever seen.
Peak usage is `stack_size - stack_free`. Use it before changing any stack
size; the httpd stack was once trimmed on intuition to a value 1,772 bytes
below its real peak, which crashed only under heavy requests.

---

## 2. Core dumps

Panics are written to a dedicated 64K flash partition, so a crash on a
headless board leaves a full backtrace behind. Nothing needs to be attached
at the moment it happens.

Read one back:

```bash
cd firmware/bacnet_bridge
esp-coredump --chip esp32 --port <PORT> info_corefile build/bacnet_bridge.elf
```

Gives the crashed task, panic reason, registers, and symbolized backtraces
for every task. Requires the ELF that produced the running image — if the
build has moved on, the addresses will not resolve.

`/api/network` also reports `last_reset_reason`, which distinguishes
`Power-on`, `Software restart` (the backstop), and `Crash (panic)` without
any tooling.

---

## 3. Local serial log

**Script:** `tools/serial_logger.py`

```bash
python3 tools/serial_logger.py <PORT> "docs/serial-logs/esp-bacnet-$(date +%Y%m%d-%H%M%S).log" &
tail -f docs/serial-logs/esp-bacnet-*.log
```

Find `<PORT>` with the `esp-hardware` skill's `serial_tool.py list`. **It
changes** depending on which physical USB port the adapter is in.

### Quirks, both confirmed repeatedly

**Opening the port resets the ESP32.** The CH340 driver pulses DTR/RTS on
`open()` regardless of what pyserial is asked to do. Not fixable from Python.
So:

- Start the logger **once** per session and leave it running.
- Running `esptool` (flash, or `chip_id` as a plain-reset trick) needs the
  port free — kill the logger first, restart it after.
- Some "reboot" events in old logs were caused by a debugging session's own
  reconnects. Read historic logs with that in mind.

**No auto-reconnect, deliberately.** A naive retry loop would reset the chip
repeatedly if the port ever legitimately dropped. If the cable is pulled the
last line will be `*** serial_logger: port lost ... exiting, not retrying ***`
— restart it by hand.

---

## 4. Router syslog (WiFi/RF events, no USB needed)

This is the channel that reveals **AP-side RSSI**, which the device itself
cannot see. It is the only way to detect a link that is asymmetric — strong
in one direction, weak in the other.

**Listener:** `tools/router_syslog_listener.py.pi-copy` (a copy of what runs
on the Pi; the live one is the authority — edit there and re-copy). Runs as a
systemd service, binds UDP on the LAN only.

**Router side:** point the remote syslog target at the listener's
`host:port`. On Asus/Merlin this is in the admin UI.

```bash
ssh <PI_HOST> "tail -f ~/esp-bacnet-diag/router-syslog.log"
```

### Verify it is actually flowing before trusting silence

The router has been observed to **silently stop forwarding** — its own logger
restarting drops the config with no error, and it can stay dark for many
hours. A quiet period is not evidence that nothing happened.

```bash
ssh <PI_HOST> "tail -5 ~/esp-bacnet-diag/router-syslog.log"
```

If the last timestamp is old, re-save the syslog target in the router UI. It
does not need to change value, just be re-applied.

### What to look for

`wlceventd` / `hostapd` lines carry RSSI at the moment of auth, assoc and
deauth. Pair those against the ESP's own `wifi:... rssi: N` line from the
same association — **they are two different measurements and a large gap
between them is itself the finding.** `rssi:0` means the AP has no current
measurement for that station, not a strong signal.

Useful reason codes: `4` (inactivity — the AP stopped hearing the station),
`15` (4-way handshake timeout), `27` (timeout), `2` (previous auth invalid).

---

## 5. Inducing an RF fault on demand

BACnet reads over the W5500 measurably degrade the WiFi link. That makes the
fault reproducible in about a minute instead of waiting hours for a window,
which is what makes physical experiments (moving the module, adding
capacitance, changing cabling) practical to evaluate.

```bash
# baseline
ping -c 30 -i 1 <BRIDGE_IP>

# under BACnet load
(for i in $(seq 1 60); do curl -s -o /dev/null http://<BRIDGE_IP>/api/status; done) &
ping -c 30 -i 1 <BRIDGE_IP>
```

Compare packet loss between the two. For a control that isolates HTTP serving
from BACnet traffic, load `/api/network` instead — it returns cached state and
performs no BACnet reads.

Note the read cache blunts this: `/api/status` only reaches the controller
when its cached values have aged past the TTL. To provoke maximum SPI traffic,
use an endpoint that reads live, or shorten the TTL for the duration of a test.

---

## 6. Quick health check, no logs needed

```bash
curl -s http://<BRIDGE_IP>/api/network | python3 -m json.tool
curl -s http://<BRIDGE_IP>/api/status  | python3 -m json.tool
curl -s http://<BRIDGE_IP>/api/debug/stacks | python3 -m json.tool
```

`/api/debug/stacks` is the first check after a suspected heap or task-start
problem. Compare `largest_free_block` with the requested stack sizes, then
look for unexpectedly small `headroom_bytes` on long-lived tasks. Only running
tasks appear: short-lived Ethernet bring-up, object-scan, and BACnet-client
tasks deregister before they delete themselves.

If BACnet fields are invalid but the device responds, probe the raw layer for
the actual error rather than a bare "invalid":

```bash
curl -s "http://<BRIDGE_IP>/api/bacnet/read?type=binary-value&instance=1&property=present-value"
```

`{"ok":false,"error":"bacnet not ready"}` means the datalink never bound —
check the serial log for the bind retry loop and for whether
`bacnet_client_task` was created at all.

---

## 7. UDP log shipping — removed, and why

An earlier design shipped every `diag_log()` line over UDP to a listener, so
the device was diagnosable without a USB cable. It was removed to take
wireless activity (a socket, `sendto()`, a DNS lookup) off the table while
diagnosing an RF problem — the diagnostic was interfering with the thing being
diagnosed.

The listener script is kept at `tools/esp_bacnet_diag_listener.py.pi-copy-UNUSED`.
To bring it back, re-add the `diag_udp_init()` / `diag_udp_send()` calls to
`diag_log()`; git history has the removed implementation. **Do not re-enable
it while investigating anything RF-related.**

---

## 8. Release-build checks

For a build that will run unattended, capture all three views before calling
it stable: `/api/network` for link and reset reason, `/api/status` for usable
BACnet values, and `/api/debug/stacks` for heap fragmentation and stack
headroom. Keep the matching ELF for any installed image so a later core dump
can be decoded. A successful flash by itself is not a stability result.

These same endpoints are the primary non-invasive checks for both supported
Ethernet profiles: the W5500 baseline and the T-ETH-Lite integrated RTL8201
profile. For the T-ETH-Lite, record the requested 32 KiB HTTP stack and both
MQTT task entries as well as free heap and largest block; do not compare only
the total-free value across the two hardware profiles.

For a read-only T-ETH smoke or soak check, use the repository helper. It
fails on a lost link/MQTT connection, missing target or required task, failed
request, or non-advancing uptime; it never writes BACnet state or firmware:

```bash
python3 tools/t_eth_runtime_check.py --samples 31 --interval 30 \
  --output /tmp/t-eth-soak.jsonl
```
