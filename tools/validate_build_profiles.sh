#!/usr/bin/env bash
# Build the primary T-ETH-Lite and explicit legacy W5500 profiles from fresh
# generated sdkconfigs. Run from any directory after sourcing ESP-IDF export.sh.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
firmware_dir="$repo_root/firmware/bacnet_bridge"
work_root=$(mktemp -d /tmp/esp-bacnet-profile-validation.XXXXXX)

cleanup() {
  if [[ "${KEEP_BUILD_ARTIFACTS:-0}" != "1" ]]; then
    rm -rf "$work_root"
  else
    printf 'Kept build artifacts at %s\n' "$work_root"
  fi
}
trap cleanup EXIT

if ! command -v idf.py >/dev/null; then
  printf '%s\n' 'idf.py is not on PATH; source ESP-IDF export.sh first.' >&2
  exit 2
fi

build_profile() {
  local name=$1
  local defaults=$2
  local profile_root="$work_root/$name"
  mkdir -p "$profile_root"
  printf '%s\n' "==> Building $name" >&2
  (
    cd "$firmware_dir"
    if [[ -n "$defaults" ]]; then
      idf.py -B "$profile_root/build" \
        -DSDKCONFIG="$profile_root/sdkconfig" \
        "-DSDKCONFIG_DEFAULTS=$defaults" \
        build
    else
      idf.py -B "$profile_root/build" \
        -DSDKCONFIG="$profile_root/sdkconfig" \
        build
    fi
  ) >&2
  shasum -a 256 "$profile_root/build/bacnet_bridge.bin" >&2
  printf '%s\n' "$profile_root"
}

teth_root=$(build_profile t_eth_lite '')
rg -q '^CONFIG_EXAMPLE_USE_INTERNAL_ETHERNET=y$' "$teth_root/sdkconfig"
rg -q '^CONFIG_SPIRAM_USE_MALLOC=y$' "$teth_root/sdkconfig"
rg -q '^CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y$' "$teth_root/sdkconfig"
rg -q '^CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768$' "$teth_root/sdkconfig"

w5500_root=$(build_profile w5500 'sdkconfig.defaults;sdkconfig.w5500.defaults')
rg -q '^CONFIG_EXAMPLE_USE_SPI_ETHERNET=y$' "$w5500_root/sdkconfig"
rg -q '^CONFIG_EXAMPLE_USE_W5500=y$' "$w5500_root/sdkconfig"
rg -q '^CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y$' "$w5500_root/sdkconfig"

printf '%s\n' 'Profile validation passed: primary T-ETH-Lite and legacy W5500.'
