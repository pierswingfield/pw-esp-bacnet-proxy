# BRIEFING — 2026-09-02T20:08:30Z

## Mission
Investigate Git repo state, build system/profiles, build tools, and partition layouts (4MB W5500 vs 16MB T-ETH-Lite, Matter OTA requirements) for ESP-BACnet.

## 🔒 My Identity
- Archetype: explorer
- Roles: survey, analysis, synthesis
- Working directory: /Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/explorer_survey_1
- Original parent: fed7dce7-ef41-4f48-9ccd-5fd974fec9ce
- Milestone: exploration_survey

## 🔒 Key Constraints
- Read-only investigation — do NOT implement
- Do NOT flash physical hardware
- `bacnet-object-catalog.json` must remain untouched
- Write only to own directory (`.agents/explorer_survey_1/`)

## Current Parent
- Conversation ID: fed7dce7-ef41-4f48-9ccd-5fd974fec9ce
- Updated: 2026-09-02T20:08:30Z

## Investigation State
- **Explored paths**: `CLAUDE.md`, `ORIGINAL_REQUEST.md`, `docs/MATTER_INTEGRATION_ASSESSMENT_AND_PLAN.md`, `docs/INTEGRATION_MODULES.md`, `firmware/bacnet_bridge/CMakeLists.txt`, `firmware/bacnet_bridge/main/CMakeLists.txt`, `sdkconfig.defaults`, `sdkconfig.t_eth_lite.defaults`, `sdkconfig.w5500.defaults`, `partitions.csv`, `tools/validate_build_profiles.sh`, `.github/workflows/publish-t-eth-lite.yml`, `components/hvac_core/`.
- **Key findings**:
  1. Branch `codex/protocol-agnostic-core` is currently checked out on HEAD (commit 46d4f68) with initial `hvac_core` extraction unstaged/untracked.
  2. `./tools/validate_build_profiles.sh` builds and passes both T-ETH-Lite and W5500 profiles cleanly.
  3. Current 4MB partition table (`partitions.csv`) limits OTA slots to 1900KB (leaving only 425KB free for T-ETH and 84KB free in 4MB flash).
  4. Matter requires enlarged OTA slots (3MB to 4MB) and expanded NVS (64KB) + factory data partition (24KB), easily accommodated by T-ETH-Lite's 16MB flash using a dedicated `partitions_t_eth_lite.csv`.
  5. W5500 legacy profile must remain on 4MB `partitions.csv` with zero Matter dependencies.
- **Unexplored areas**: None for this survey scope.

## Key Decisions Made
- Recommended dedicated `partitions_t_eth_lite.csv` (4MB OTA slots, 64KB NVS, 24KB matter_fctry, 128KB coredump) referenced via `sdkconfig.t_eth_lite.defaults`.
- Recommended modularizing Matter adapter in a dedicated component with Kconfig gating to guarantee W5500 isolation.

## Artifact Index
- `/Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/explorer_survey_1/BRIEFING.md` — persistent memory
- `/Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/explorer_survey_1/progress.md` — heartbeat and progress tracking
- `/Users/pierswingfield/Desktop/AI Projects/Random/esp-bacnet/.agents/explorer_survey_1/handoff.md` — final 5-component report
