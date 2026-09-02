#!/usr/bin/env python3
"""Read-only T-ETH-Lite runtime smoke and soak checker.

The checker polls the four production diagnostics and fails closed on a
request failure, unexpected reset, lost Ethernet/MQTT connection, missing
BACnet target, or missing/undersized T-ETH task configuration.  It performs no
BACnet write, MQTT publish, firmware update, or device restart.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


ENDPOINTS = ("/api/network", "/api/status", "/api/mqtt/config", "/api/debug/stacks")
EXPECTED_STACKS = {"httpd": 32768, "mqtt_command": 24576, "mqtt_state": 16384}


def get_json(base_url: str, endpoint: str, timeout: float) -> dict[str, Any]:
    with urllib.request.urlopen(f"{base_url}{endpoint}", timeout=timeout) as response:
        return json.load(response)


def validate(sample: dict[str, dict[str, Any]], prior_uptime: int | None) -> list[str]:
    errors: list[str] = []
    network = sample["/api/network"]
    mqtt = sample["/api/mqtt/config"]
    stacks = sample["/api/debug/stacks"]

    if not network.get("eth_connected"):
        errors.append("Ethernet link is not connected")
    if not network.get("bacnet_target_name") or not network.get("bacnet_target_ip"):
        errors.append("BACnet target was not discovered")
    if network.get("last_reset_reason") not in {"Power-on", "Software restart"}:
        errors.append(f"unexpected reset reason: {network.get('last_reset_reason')!r}")
    uptime = network.get("uptime_seconds")
    if prior_uptime is not None and (not isinstance(uptime, int) or uptime <= prior_uptime):
        errors.append(f"uptime did not advance: {prior_uptime!r} -> {uptime!r}")
    if not mqtt.get("connected"):
        errors.append("MQTT is not connected")

    task_by_name = {task.get("name"): task for task in stacks.get("tasks", [])}
    for name, requested in EXPECTED_STACKS.items():
        task = task_by_name.get(name)
        if not task:
            errors.append(f"missing {name} task")
            continue
        if task.get("requested") != requested:
            errors.append(f"{name} requested {task.get('requested')!r}, expected {requested}")
        if not isinstance(task.get("headroom_bytes"), int) or task["headroom_bytes"] <= 0:
            errors.append(f"{name} has non-positive headroom: {task.get('headroom_bytes')!r}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://192.168.1.3")
    parser.add_argument("--samples", type=int, default=1, help="number of complete endpoint samples")
    parser.add_argument("--interval", type=float, default=30.0, help="seconds between samples")
    parser.add_argument("--timeout", type=float, default=8.0, help="per-request timeout in seconds")
    parser.add_argument("--output", type=Path, help="optional JSONL evidence file")
    args = parser.parse_args()
    if args.samples < 1 or args.interval < 0 or args.timeout <= 0:
        parser.error("--samples must be >= 1, --interval >= 0, and --timeout > 0")

    output = args.output.open("w", encoding="utf-8") if args.output else None
    failures: list[str] = []
    prior_uptime: int | None = None
    try:
        for number in range(1, args.samples + 1):
            row: dict[str, Any] = {"sample": number, "timestamp": time.time(), "results": {}}
            for endpoint in ENDPOINTS:
                try:
                    row["results"][endpoint] = get_json(args.base_url, endpoint, args.timeout)
                except (OSError, ValueError, urllib.error.URLError) as exc:
                    failures.append(f"sample {number} {endpoint}: {exc}")
                    row["results"][endpoint] = {"error": str(exc)}
            if not failures or all(endpoint in row["results"] and "error" not in row["results"][endpoint] for endpoint in ENDPOINTS):
                failures.extend(f"sample {number}: {error}" for error in validate(row["results"], prior_uptime))
                uptime = row["results"]["/api/network"].get("uptime_seconds")
                prior_uptime = uptime if isinstance(uptime, int) else prior_uptime
            if output:
                output.write(json.dumps(row, sort_keys=True) + "\n")
                output.flush()
            if number < args.samples:
                time.sleep(args.interval)
    finally:
        if output:
            output.close()

    if failures:
        print("T-ETH runtime check FAILED:", file=sys.stderr)
        print("\n".join(f"- {failure}" for failure in failures), file=sys.stderr)
        return 1
    print(f"T-ETH runtime check passed: {args.samples} sample(s), {args.samples * len(ENDPOINTS)} requests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
