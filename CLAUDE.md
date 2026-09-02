# ESP-BACnet agent guide

`AGENTS.md` points here so Codex and Claude use the same project contract.

## Hardware and branch policy

- **T-ETH-Lite is the primary implementation.** Its integrated RTL8201 PHY,
  16 MB flash and PSRAM profile are the default build.
- **W5500 is legacy/recovery only.** Keep it buildable as an explicit profile;
  do not change its pins or make it the default.
- Keep only one bridge on the isolated `10.0.3.x` BACnet segment at a time:
  both profiles use the same static address.

## Build and verification

Source ESP-IDF 5.3.1 before building. The normal T-ETH-Lite command is:

```bash
cd firmware/bacnet_bridge
idf.py -B build-t-eth -DSDKCONFIG=build-t-eth/sdkconfig \
  -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.t_eth_lite.defaults' build
```

Before any release, run `tools/validate_build_profiles.sh`. It creates clean
T-ETH-Lite and legacy W5500 builds. Do not claim a hardware change passed from
a build alone: live evidence must name the flashed image and observed result.

## OTA release contract

- The firmware version in `firmware/bacnet_bridge/CMakeLists.txt` must match
  the release tag `vX.Y.Z-t-eth-lite`.
- Push that annotated tag only after it is merged to `master` and profile
  validation passes.
- GitHub Actions then builds both profiles, publishes the T-ETH-Lite OTA
  binary, computes its SHA-256, and updates `releases/t-eth-lite.json`.
- Do not manually edit manifest `image_url` or `sha256` for future releases.
- The Update page is a user-initiated GitHub check followed by download and
  authenticated LAN OTA upload; it is not unattended device auto-install.

## Current accepted live evidence

T-ETH-Lite 0.3.0 has passed live Ethernet unplug/replug recovery, immediate
Health-page link-down reporting, browser and CLI object scan, and LAN OTA.
The published release asset is the OTA application binary, not a full USB
flash image. Keep the deliberate rollback test and longer 24-hour/72-hour
soaks as separate release-acceptance evidence.

## Documentation and local artefacts

- Keep README and `docs/ARCHITECTURE_AND_API.md` current-facing.
- Keep `docs/T_ETH_LITE_V2_ARCHITECTURE_AND_BUILD.md` as concise historical
  evidence plus the current acceptance record; do not leave completed work in
  a forward-looking backlog.
- Leave dated diagnosis documents as historical records unless correcting a
  factual error.
- `bacnet-object-catalog.json` is a user-generated local commissioning output;
  do not add, delete or overwrite it unless explicitly asked.
