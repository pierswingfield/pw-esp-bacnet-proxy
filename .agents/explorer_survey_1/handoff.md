# Build & Git System Explorer Handoff Report

## 1. Observation

### 1.1 Git Repository State
- **Current Branch**: `codex/protocol-agnostic-core` (verified via `git status` and `git branch -a`).
- **Commit HEAD**: `46d4f68ab6147dc9f1bec9575bbe20e764a2e290` (`docs: record T-ETH-Lite acceptance and release contract`), aligned with `origin/master` and `master`.
- **Existing Local Branches**:
  - `archive/round-1-2-3-experimental`
  - `codex/protocol-agnostic-core` (ACTIVE)
  - `codex/t-eth-lite-hardware-proof`
  - `fix/wifi-stability-and-ram-budget`
  - `legacy/w5500`
  - `master`
- **Existing Remote Branches**:
  - `origin/HEAD -> origin/master`
  - `origin/codex/t-eth-lite-hardware-proof`
  - `origin/fix/wifi-stability-and-ram-budget`
  - `origin/legacy/w5500`
  - `origin/master`
- **Release & Milestone Tags**:
  - `archive/next-build-2026-08-16`
  - `v0.3.0-t-eth-lite` (at commit `86bb57a`)
  - `w5500-final-2026-09-02`
- **Working Tree Status**:
  - Uncommitted modified files: `README.md`, `docs/ARCHITECTURE_AND_API.md`, `firmware/bacnet_bridge/main/CMakeLists.txt`, `firmware/bacnet_bridge/main/main.c`.
  - Untracked files: `.agents/`, `bacnet-object-catalog.json` (protected commissioning catalog), `docs/INTEGRATION_MODULES.md`, `docs/MATTER_INTEGRATION_ASSESSMENT_AND_PLAN.md`, `firmware/bacnet_bridge/components/hvac_core/`.

### 1.2 Build System & Profiles in `firmware/bacnet_bridge/`
- `CMakeLists.txt` (lines 1–10):
  ```cmake
  cmake_minimum_required(VERSION 3.16)
  if(NOT DEFINED SDKCONFIG_DEFAULTS)
      set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.t_eth_lite.defaults"
          CACHE STRING "Semicolon-separated sdkconfig defaults files")
  endif()
  include($ENV{IDF_PATH}/tools/cmake/project.cmake)
  project(bacnet_bridge VERSION 0.3.0)
  ```
- `main/CMakeLists.txt` (lines 1–5):
  ```cmake
  idf_component_register(SRCS "main.c"
                         INCLUDE_DIRS "."
                         EMBED_FILES "root.html" "manage.html" "status.html" "health.html" "reset.html" "update.html" "objects.html" "wizard.html" "mqtt.html"
                         REQUIRES ethernet_init bacnet_client hvac_core esp_netif esp_eth dns_server nvs_flash esp_wifi esp_http_server mqtt app_update mbedtls mdns)
  ```
- `sdkconfig.defaults` (lines 1–37): Defines base shared config: `CONFIG_PARTITION_TABLE_CUSTOM=y`, `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"`, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`, `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096`.
- `sdkconfig.t_eth_lite.defaults` (lines 1–38):
  - Target: ESP32-WROVER-E (16MB Flash, 8MB PSRAM).
  - Flags: `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`, `CONFIG_EXAMPLE_USE_INTERNAL_ETHERNET=y`, `CONFIG_EXAMPLE_ETH_PHY_RTL8201=y`, `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_USE_MALLOC=y`, `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`, `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`.
- `sdkconfig.w5500.defaults` (lines 1–19):
  - Target: ESP32-WROOM-32D (4MB Flash, No PSRAM).
  - Flags: `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y`, `CONFIG_EXAMPLE_USE_INTERNAL_ETHERNET=n`, `CONFIG_EXAMPLE_USE_SPI_ETHERNET=y`, `CONFIG_EXAMPLE_USE_W5500=y`.

### 1.3 Profile Validation Script & CI Workflows
- `tools/validate_build_profiles.sh` (lines 1–59):
  - Builds `t_eth_lite` profile into isolated temporary path `/tmp/esp-bacnet-profile-validation.XXXXXX/t_eth_lite`. Asserts `CONFIG_EXAMPLE_USE_INTERNAL_ETHERNET=y`, `CONFIG_SPIRAM_USE_MALLOC=y`, `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`, `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`.
  - Builds `w5500` profile into `/tmp/esp-bacnet-profile-validation.XXXXXX/w5500`. Asserts `CONFIG_EXAMPLE_USE_SPI_ETHERNET=y`, `CONFIG_EXAMPLE_USE_W5500=y`, `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y`.
  - Computes SHA256 checksums on built binaries.
  - Live execution result: Exit code 0, all assertions passed.
- `.github/workflows/publish-t-eth-lite.yml` (lines 30–39): Uses ESP-IDF v5.3.1 docker container (`espressif/idf:v5.3.1`) to build both `build-release` and `build-w5500-release`.

### 1.4 Partition Layout Analysis
- Current `partitions.csv` (lines 7–16):
  ```csv
  nvs,      data, nvs,     ,        0x6000,
  otadata,  data, ota,     ,        0x2000,
  phy_init, data, phy,     ,        0x1000,
  ota_0,    app,  ota_0,   ,        1900K,
  ota_1,    app,  ota_1,   ,        1900K,
  coredump, data, coredump, ,      64K,
  ```
- Binary layout parsed by `gen_esp32part.py`:
  - `nvs`: offset `0x9000`, size `24K` (`0x6000`), ends `0xF000`.
  - `otadata`: offset `0xF000`, size `8K` (`0x2000`), ends `0x11000`.
  - `phy_init`: offset `0x11000`, size `4K` (`0x1000`), ends `0x12000`.
  - `ota_0`: offset `0x20000`, size `1900K` (`0x1DB000` = 1,945,600 B), ends `0x1FB000`.
  - `ota_1`: offset `0x200000`, size `1900K` (`0x1DB000` = 1,945,600 B), ends `0x3DB000`.
  - `coredump`: offset `0x3DB000`, size `64K` (`0x10000`), ends `0x3EB000` (4,108,288 B).
  - Total flash used: ~3.91 MB out of 4.00 MB (`0x400000`). Free flash at tail: `84 KB` (`0x15000`).

### 1.5 Binary Sizes & Headroom Measurements
- **W5500 Profile**:
  - `bacnet_bridge.bin` binary size: `0x165320` (1,463,072 bytes, 1.395 MB).
  - Free space in 1900KB slot (`0x1db000`): `0x75ce0` bytes (482,528 bytes, 25% free).
- **T-ETH-Lite Profile**:
  - `bacnet_bridge.bin` binary size: `0x173400` (1,520,640 bytes, 1.450 MB).
  - Free space in 1900KB slot (`0x1db000`): `0x67c00` bytes (424,960 bytes, 22% free).
  - RAM usage: Flash Code `.text` 875,797 B, Flash Data `.rodata` 510,016 B, IRAM 116,118 B (88.6%), DRAM 99,344 B (55.0%).

---

## 2. Logic Chain

1. **Git State (Task 1)**:
   - Observation: `git status` returned `On branch codex/protocol-agnostic-core`.
   - Logic: The required working branch is already active and reflects current work; no branch checkout or recreation is necessary.

2. **Build Configuration & Isolation (Task 2 & 3)**:
   - Observation: `firmware/bacnet_bridge/CMakeLists.txt` sets `SDKCONFIG_DEFAULTS` to T-ETH-Lite by default. `tools/validate_build_profiles.sh` and CI explicitly supply defaults for W5500.
   - Logic: Both build configurations are fully functional and pass validation independently. Any Matter dependencies must be isolated to T-ETH-Lite to prevent breaking the W5500 profile build.

3. **Partition Table Limits on 4MB Flash (Task 4)**:
   - Observation: `partitions.csv` consumes 3.91MB of 4MB flash with two 1900KB OTA slots, leaving only 84KB unpartitioned flash.
   - Observation: Matter stacks (Connected Home IP / ESP-Matter, libstdc++, crypto, BLE/NimBLE, mDNS) add 800KB to 1.5MB to the binary size, pushing total image size to 2.2MB–2.8MB.
   - Logic: A 1900KB slot cannot fit a Matter binary (overflow by 300KB–900KB+). Dual 2.5MB slots require 5MB+, which exceeds physical 4MB flash. Therefore, 4MB W5500 cannot support Matter.

4. **16MB Flash Partition Opportunity (Task 4)**:
   - Observation: T-ETH-Lite has 16MB physical flash (`CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`) and 8MB PSRAM.
   - Logic: T-ETH-Lite can easily accommodate two 4096KB (4MB) OTA slots, a 64KB NVS partition (for Matter fabrics/credentials), a 24KB `matter_fctry` partition (subtype `0x99`), and a 128KB coredump partition, using ~8.31MB total and leaving 7.68MB free headroom.
   - Logic: Specifying `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_t_eth_lite.csv"` in `sdkconfig.t_eth_lite.defaults` isolates the 16MB table to T-ETH-Lite without modifying W5500's 4MB `partitions.csv`.

---

## 3. Caveats

1. **Physical Flashing**: Per project policy and prompt constraints, no physical flashing or live OTA push was executed during this survey.
2. **ESP-Matter Component Resolution**: In ESP-IDF v5.3.1, adding `espressif/esp-matter` via IDF Component Manager (`idf_component.yml`) requires network fetching and CMake registration. The Matter adapter should be encapsulated in a dedicated component directory (`components/matter_adapter` or `components/hvac_matter`) so W5500 does not parse or require Matter packages.
3. **Protected Artifact**: `bacnet-object-catalog.json` remains completely unmodified and untracked as required.

---

## 4. Conclusion

1. **Git State**: Repository is on branch `codex/protocol-agnostic-core` at commit `46d4f68`, ready for continuing R1 (BACnet worker queue) and R2 (Matter POC).
2. **Build System & Validation**: Both T-ETH-Lite and W5500 build profiles compile cleanly and pass `tools/validate_build_profiles.sh`.
3. **Partition Strategy**:
   - Keep `partitions.csv` (4MB layout, 1900KB slots) for legacy W5500 recovery.
   - Create `partitions_t_eth_lite.csv` for T-ETH-Lite with:
     - `nvs`: `64K` (`0x10000`)
     - `otadata`: `8K` (`0x2000`)
     - `phy_init`: `4K` (`0x1000`)
     - `matter_fctry`: `24K` (`0x6000`, type `data`, subtype `0x99`)
     - `ota_0`: `4096K` (`4M`) at `0x30000`
     - `ota_1`: `4096K` (`4M`) at `0x430000`
     - `coredump`: `128K` (`0x20000`) at `0x830000`
   - Set `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_t_eth_lite.csv"` in `sdkconfig.t_eth_lite.defaults`.
4. **Matter Architectural Placement**:
   - Enforce single active integration via `hvac_core_integration_get()` (NONE, MQTT, MATTER).
   - Gate Matter initialization to ensure MQTT task stack and client allocations are deferred when Matter is selected.

---

## 5. Verification Method

To independently verify these findings:

1. **Git Status & Branch**:
   ```bash
   git status
   git branch -a
   ```
   *Expected: On branch `codex/protocol-agnostic-core`, `bacnet-object-catalog.json` unmodified.*

2. **Dual Profile Build Regression**:
   ```bash
   source /Users/pierswingfield/esp/esp-idf/export.sh
   ./tools/validate_build_profiles.sh
   ```
   *Expected: Script exits with code 0 and outputs "Profile validation passed: primary T-ETH-Lite and legacy W5500."*

3. **Partition Table Verification**:
   ```bash
   python3 /Users/pierswingfield/esp/esp-idf/components/partition_table/gen_esp32part.py -v firmware/bacnet_bridge/partitions.csv /tmp/ptable.bin
   python3 /Users/pierswingfield/esp/esp-idf/components/partition_table/gen_esp32part.py /tmp/ptable.bin
   ```
   *Expected: Displays 1900K `ota_0` and `ota_1` slots fitting within 4MB boundary.*

4. **Flash & RAM Size Measurement**:
   ```bash
   cd firmware/bacnet_bridge
   idf.py -B build-t-eth -DSDKCONFIG=build-t-eth/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.t_eth_lite.defaults" size
   ```
   *Expected: Confirms current image size ~1.45MB, DRAM usage ~55%, IRAM ~88.6%.*
