# Progress Log — Milestones M3 & M4

Last visited: 2026-09-02T20:38:55Z

## Status
- **Milestone M3 & M4 COMPLETE**:
  1. `partitions_t_eth_lite.csv` created and validated for 16MB flash with dual 4MB OTA slots, 64K NVS, 24K matter_fctry, 128K coredump.
  2. `sdkconfig.t_eth_lite.defaults` configured with `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_t_eth_lite.csv"` and `CONFIG_ENABLE_ESP_MATTER=y`.
  3. `sdkconfig.w5500.defaults` configured with `CONFIG_ENABLE_ESP_MATTER=n` while leaving 4MB `partitions.csv` untouched.
  4. `components/matter_adapter` created with full CSA Matter Thermostat Cluster 0x0201 mapping (Local Temp 0x0000, Cooling/Heating Setpoints 0x0011/0x0012, System Mode 0x001C) routing strictly through canonical `hvac_core`.
  5. Web REST API `/api/integration` (GET & POST) implemented with dynamic mutual exclusion and resource unallocation/re-allocation.
  6. Dual build profile compilation validated via `./tools/validate_build_profiles.sh` with 0 compile errors.
  7. Full test suite passes: `pytest` 173/173 PASS, `python3 tests/run_e2e_tests.py` 130/130 PASS.
  8. Commissioning catalog `bacnet-object-catalog.json` remains strictly untouched. No physical flashing performed.
