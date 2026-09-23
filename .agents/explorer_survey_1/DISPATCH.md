## 2026-09-02T20:04:01Z
You are the Build & Git System Explorer for the ESP-BACnet project.
Working directory: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/explorer_survey_1
Original User Request: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/ORIGINAL_REQUEST.md

Please read /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/ORIGINAL_REQUEST.md first.

Task:
1. Investigate the Git repository state (current branch, available branches, uncommitted changes, tags). Determine if branch `codex/protocol-agnostic-core` exists, is checked out, or needs to be created/switched to. Note: You are read-only; report git status and recommendations in your handoff report.
2. Investigate the build system and configuration files in `firmware/bacnet_bridge/` (CMakeLists.txt, sdkconfig.defaults, sdkconfig.t_eth_lite.defaults, sdkconfig.w5500.defaults, partitions.csv, etc.).
3. Investigate the dual profile validation script `./tools/validate_build_profiles.sh` and any other build tools/scripts in `tools/`.
4. Analyze the partition layout for 4MB (W5500) vs 16MB (T-ETH-Lite), specifically the OTA partition sizes, NVS, coredump, and Matter requirements (Matter typically needs ~1.5MB to 2.5MB per OTA app slot).
5. Document all findings with file paths, line numbers, and concrete architectural recommendations in `/Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/explorer_survey_1/handoff.md` and keep `progress.md` updated.
