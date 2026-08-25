#!/usr/bin/env python3
"""Continuous local serial capture - no network involved. Appends timestamped
lines to a local file until killed or the port drops.

IMPORTANT: opening this port resets the ESP32 (confirmed 2026-08-23 - the
CH340 driver on macOS pulses DTR/RTS during open() regardless of what values
you request, at least with this adapter/driver combo). So: run this ONCE,
leave it running, and don't reconnect casually - every reconnect is a reset.
If the port drops for a real reason (cable unplugged, adapter power-cycled),
this exits with a clear message instead of silently retrying (retrying would
just reset the board again on every attempt)."""
import datetime
import sys
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-110"
BAUD = 115200
OUT_PATH = sys.argv[2] if len(sys.argv) > 2 else "serial.log"

def log(f, text):
    ts = datetime.datetime.now().isoformat(timespec="milliseconds")
    f.write(f"{ts} {text}\n")

def main():
    print(f"logging {PORT} -> {OUT_PATH}", flush=True)
    ser = serial.Serial(PORT, BAUD, timeout=1)
    with open(OUT_PATH, "a", buffering=1) as f:
        log(f, "*** serial_logger: port opened (this resets the chip - known adapter quirk) ***")
        buf = b""
        try:
            while True:
                chunk = ser.read(4096)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode("utf-8", errors="replace").rstrip("\r")
                    log(f, text)
        except (serial.SerialException, OSError) as e:
            log(f, f"*** serial_logger: port lost ({e}) - exiting, not retrying ***")
            raise

if __name__ == "__main__":
    main()
