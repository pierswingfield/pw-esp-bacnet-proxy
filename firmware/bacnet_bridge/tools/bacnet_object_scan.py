#!/usr/bin/env python3
"""Run the bridge's BACnet object scan and save the resulting catalog.

This is intentionally a client of the supported HTTP API, so it works over
Wi-Fi without a serial cable and produces a shareable JSON commissioning
artifact. The bridge must have its BACnet Ethernet cable connected.
"""
import argparse
import json
import sys
import time
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


def request_json(url, method="GET"):
    request = Request(url, method=method, headers={"Accept": "application/json"})
    with urlopen(request, timeout=15) as response:
        return json.loads(response.read().decode("utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_url", help="Bridge URL, e.g. http://10.0.3.16")
    parser.add_argument("--output", type=Path, default=Path("bacnet-object-catalog.json"))
    parser.add_argument("--poll-seconds", type=float, default=1.5)
    args = parser.parse_args()
    base_url = args.base_url.rstrip("/")

    try:
        network = request_json(base_url + "/api/network")
        if not network.get("eth_connected"):
            raise RuntimeError("Ethernet link is down; connect BACnet Ethernet before scanning")
        started = request_json(base_url + "/api/objects/scan-start", method="POST")
        if not started.get("ok"):
            raise RuntimeError(started.get("error", "scan could not be started"))
        while True:
            status = request_json(base_url + "/api/objects/scan-status")
            state = status.get("state", "unknown")
            print("%s: %s/%s (%s%%), %s objects" % (
                state, status.get("current", 0), status.get("total", 0),
                status.get("percent", 0), status.get("count", 0)), flush=True)
            if state == "complete":
                break
            if state == "error":
                raise RuntimeError(status.get("error", "scan failed"))
            time.sleep(args.poll_seconds)
        catalog = request_json(base_url + "/api/objects")
    except (HTTPError, URLError, ValueError, RuntimeError) as exc:
        print("Scan failed: %s" % exc, file=sys.stderr)
        return 1

    args.output.write_text(json.dumps(catalog, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("Saved %d objects to %s" % (catalog.get("count", 0), args.output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
